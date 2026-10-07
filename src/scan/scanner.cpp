#include <GhidraEngine/scan/scanner.hpp>

#include <GhidraEngine/hash/blake3.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace GhidraEngine {

namespace {

void check_stop(const std::stop_token &stop) {
    if (stop.stop_requested()) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_canceled),
            "Media scan cancelled"
        );
    }
}

bool directory_link(const std::filesystem::path &path, std::error_code &error) {
#ifdef _WIN32
    // MinGW does not classify directory junctions as filesystem symlinks.
    const auto attributes = GetFileAttributesW(path.c_str());

    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }

    error.clear();
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
#endif
}

bool inside(const std::filesystem::path &path, const std::filesystem::path &directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return false;
    }

    for (auto ancestor = path; !ancestor.empty();) {
        if (std::filesystem::equivalent(ancestor, directory, error)) {
            return true;
        }

        const auto parent = ancestor.parent_path();
        if (parent == ancestor) {
            break;
        }

        ancestor = parent;
    }

    return false;
}

struct FileState {
    std::uintmax_t size;
    std::filesystem::file_time_type modified;
    bool operator==(const FileState &) const = default;
};

FileState file_state(const std::filesystem::path &path) {
    if (!std::filesystem::is_regular_file(path)) {
        throw std::filesystem::filesystem_error(
            "Scan input must remain a regular file",
            path,
            std::make_error_code(std::errc::invalid_argument)
        );
    }

    return {
        std::filesystem::file_size(path),
        std::filesystem::last_write_time(path)
    };
}

MediaFingerprint generate(
    const std::filesystem::path &path,
    const FingerprintSettings &settings,
    const std::stop_token &stop
) {
    std::string image_error;

    if (settings.kinds != MediaKinds::Videos) {
        try {
            return fingerprint_image(decode_image(path, settings.image, stop), settings.transforms);
        } catch (const std::filesystem::filesystem_error &error) {
            if (settings.kinds == MediaKinds::Images) {
                throw;
            }

            image_error = error.what();
        }
    }

    try {
        return fingerprint_video(path, settings.video, stop);
    } catch (const std::filesystem::filesystem_error &error) {
        if (image_error.empty()) {
            throw;
        }

        throw std::runtime_error("Image probe: " + image_error + "; video probe: " + error.what());
    }
}

std::vector<std::filesystem::path> normalize_roots(const std::span<const std::filesystem::path> roots) {
    std::vector<std::filesystem::path> normalized_roots;

    for (const auto &root : roots) {
        if (root.empty() || root.native().find(std::filesystem::path::value_type{}) !=
                                std::filesystem::path::string_type::npos) {
            throw std::invalid_argument("Invalid scan root path");
        }

        auto path = std::filesystem::absolute(root);
        const auto status = std::filesystem::status(path);

        while (path.has_relative_path() && (path.filename().empty() || path.filename() == ".")) {
            path = path.parent_path();
        }

        std::error_code error;
        const bool linked_directory =
            std::filesystem::is_directory(status) && directory_link(path, error);

        if (error) {
            throw std::filesystem::filesystem_error("Cannot inspect scan root", path, error);
        }

        if ((!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status)) ||
            linked_directory) {
            throw std::filesystem::filesystem_error(
                "Scan root must be a file or real directory",
                path,
                std::make_error_code(std::errc::invalid_argument)
            );
        }

        // Resolve parent components through the filesystem, preserving final file symlinks.
        if (std::filesystem::is_directory(status)) {
            path = std::filesystem::canonical(path);
        } else {
            path = std::filesystem::canonical(path.parent_path()) / path.filename();
        }

        normalized_roots.push_back(std::move(path));
    }

    return normalized_roots;
}

std::set<std::filesystem::path> collect_paths(
    const std::span<const std::filesystem::path> roots,
    const std::size_t max_files,
    const FingerprintCache *cache,
    const std::stop_token &stop,
    std::vector<ScanIssue> &issues,
    const ScanProgressCallback &progress
) {
    std::set<std::filesystem::path> paths;
    const auto add = [&](const std::filesystem::path &path) {
        const bool added = paths.insert(path).second;

        if (paths.size() > max_files) {
            throw std::length_error("Scan exceeds unique file budget");
        }

        if (added && progress) {
            progress({ScanProgressStage::Enumeration, paths.size(), 0, {}, path});
        }
    };

    for (const auto &root : roots) {
        check_stop(stop);

        if (cache && inside(root, cache->directory())) {
            continue;
        }

        if (std::filesystem::is_regular_file(root)) {
            add(root);
            continue;
        }

        std::error_code enumeration_error;
        std::filesystem::recursive_directory_iterator iterator(root, enumeration_error);

        if (enumeration_error) {
            issues.push_back({root, ScanStage::Enumeration, enumeration_error.message()});
            continue;
        }

        const std::filesystem::recursive_directory_iterator end;

        while (iterator != end) {
            check_stop(stop);
            const auto path = iterator->path().lexically_normal();
            std::error_code error;
            const auto status = iterator->status(error);
            const bool linked_directory = !error && std::filesystem::is_directory(status) &&
                                          directory_link(path, error);

            if (error) {
                issues.push_back({path, ScanStage::Enumeration, error.message()});
                iterator.disable_recursion_pending();
            } else if (linked_directory ||
                       (cache && std::filesystem::is_directory(status) &&
                        inside(path, cache->directory()))) {
                iterator.disable_recursion_pending();
            } else if (std::filesystem::is_regular_file(status)) {
                add(path);
            }

            iterator.increment(error);

            if (error) {
                issues.push_back({path, ScanStage::Enumeration, error.message()});
            }
        }
    }

    return paths;
}

void load_cached_fingerprint(
    ScannedFile &file,
    const Blake3Digest &content,
    const FingerprintSettings &settings,
    const Blake3Digest &settings_key,
    const FingerprintCache *cache,
    std::vector<ScanIssue> &issues
) {
    if (!cache) {
        return;
    }

    try {
        file.fingerprint = cache->load(content, settings_key, settings.video.max_frames);
        file.cache_hit = file.fingerprint.has_value();
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &error) {
        issues.push_back({file.path, ScanStage::CacheRead, error.what()});
    }
}

void store_cached_fingerprint(
    const ScannedFile &file,
    const MediaFingerprint &fingerprint,
    const Blake3Digest &content,
    const Blake3Digest &settings_key,
    const FingerprintCache *cache,
    std::vector<ScanIssue> &issues
) {
    if (!cache || file.cache_hit) {
        return;
    }

    try {
        cache->store(content, settings_key, fingerprint);
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &error) {
        issues.push_back({file.path, ScanStage::CacheWrite, error.what()});
    }
}

void process_file(
    ScannedFile &file,
    const FingerprintSettings &settings,
    const bool generate_fingerprints,
    const Blake3Digest &settings_key,
    const FingerprintCache *cache,
    const std::stop_token &stop,
    std::vector<ScanIssue> &issues
) {
    std::optional<FileState> before;

    try {
        before = file_state(file.path);
        file.size = before->size;
        file.digest = hash_blake3_file(file.path, stop);

        if (generate_fingerprints) {
            load_cached_fingerprint(file, *file.digest, settings, settings_key, cache, issues);
        }

        check_stop(stop);

        if (generate_fingerprints && !file.fingerprint) {
            file.fingerprint = generate(file.path, settings, stop);
        }

        check_stop(stop);

        if (file_state(file.path) != *before) {
            file.digest.reset();
            file.fingerprint.reset();
            file.cache_hit = false;
            throw std::runtime_error("File changed while hashing/fingerprinting");
        }

        if (generate_fingerprints) {
            store_cached_fingerprint(
                file,
                *file.fingerprint,
                *file.digest,
                settings_key,
                cache,
                issues
            );
        }
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &error) {
        check_stop(stop);
        file.fingerprint.reset();
        file.cache_hit = false;

        if (file.digest) {
            try {
                if (!before || *before != file_state(file.path)) {
                    file.digest.reset();
                }
            } catch (const std::filesystem::filesystem_error &) {
                file.digest.reset();
            }
        }

        issues.push_back({file.path, ScanStage::Fingerprint, error.what()});
    }
}

}

ScanResult scan_media(
    const std::span<const std::filesystem::path> roots,
    const ScanOptions &options,
    const FingerprintCache *cache,
    const std::stop_token &stop
) {
    return scan_media(roots, options, cache, stop, {});
}

ScanResult scan_media(
    const std::span<const std::filesystem::path> roots,
    const ScanOptions &options,
    const FingerprintCache *cache,
    const std::stop_token &stop,
    const ScanProgressCallback &progress
) {
    const auto settings_key = options.generate_fingerprints
        ? fingerprint_settings_key(options.fingerprints)
        : Blake3Digest{};

    if (roots.empty() || options.workers == 0 || options.workers > 256 || options.max_files == 0) {
        throw std::invalid_argument("Scan needs roots, 1..256 workers and a positive file budget");
    }

    check_stop(stop);
    const auto normalized_roots = normalize_roots(roots);
    ScanResult result;
    // An exact-only scan ignores the cache, including its directory exclusion.
    const auto active_cache = options.generate_fingerprints ? cache : nullptr;

    if (progress) {
        progress({ScanProgressStage::Enumeration, 0, 0, {}, {}});
    }
    const auto paths = collect_paths(
        normalized_roots,
        options.max_files,
        active_cache,
        stop,
        result.issues,
        progress
    );

    if (progress) {
        progress({ScanProgressStage::Processing, paths.size(), 0, paths.size(), {}});
    }

    result.files.reserve(paths.size());

    for (const auto &path : paths) {
        result.files.push_back({MediaId{result.files.size()}, path, 0, {}, {}, false});
    }

    std::vector<std::vector<ScanIssue>> issues(result.files.size());
    std::atomic<std::size_t> next{0};
    std::exception_ptr fatal;
    std::mutex fatal_mutex;
    std::mutex progress_mutex;
    std::size_t completed = 0;

    std::stop_source cancellation;
    const std::stop_callback callback(stop, [&] {
        cancellation.request_stop();
    });

    const auto token = cancellation.get_token();
    const auto worker = [&] {
        try {
            while (!token.stop_requested()) {
                const auto position = next.fetch_add(1, std::memory_order_relaxed);

                if (position >= result.files.size()) {
                    return;
                }

                process_file(
                    result.files[position],
                    options.fingerprints,
                    options.generate_fingerprints,
                    settings_key,
                    active_cache,
                    token,
                    issues[position]
                );

                if (progress) {
                    std::lock_guard lock(progress_mutex);
                    check_stop(token);
                    progress(ScanProgress{
                        .stage = ScanProgressStage::Processing,
                        .discovered = paths.size(),
                        .completed = ++completed,
                        .total = paths.size(),
                        .path = result.files[position].path
                    });
                }
            }
        } catch (...) {
            std::lock_guard lock(fatal_mutex);

            if (!fatal) {
                fatal = std::current_exception();
            }

            cancellation.request_stop();
        }
    };

    std::vector<std::jthread> workers;
    const auto count = std::min(options.workers, result.files.size());
    workers.reserve(count);

    try {
        for (std::size_t i = 0; i < count; ++i) {
            workers.emplace_back(worker);
        }
    } catch (...) {
        cancellation.request_stop();
        throw;
    }

    for (auto &thread : workers) {
        thread.join();
    }

    if (fatal) {
        std::rethrow_exception(fatal);
    }

    check_stop(stop);

    for (auto &file_issues : issues) {
        result.issues.insert(
            result.issues.end(),
            std::make_move_iterator(file_issues.begin()),
            std::make_move_iterator(file_issues.end())
        );
    }

    std::ranges::sort(result.issues, [](const auto &a, const auto &b) {
        return std::tie(a.path, a.stage, a.message) < std::tie(b.path, b.stage, b.message);
    });

    return result;
}

std::vector<std::vector<MediaId>> exact_duplicate_groups(std::span<const ScannedFile> files) {
    std::map<Blake3Digest, std::vector<MediaId>> groups;

    for (const auto &file : files) {
        if (file.digest) {
            groups[*file.digest].push_back(file.id);
        }
    }

    std::vector<std::vector<MediaId>> result;

    for (auto &[digest, ids] : groups) {
        (void)digest;
        std::ranges::sort(ids);
        ids.erase(std::ranges::unique(ids).begin(), ids.end());

        if (ids.size() > 1) {
            result.push_back(std::move(ids));
        }
    }

    std::ranges::sort(result);

    return result;
}
}
