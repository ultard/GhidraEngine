#include "core/enumerate.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/glob.hpp"
#include "core/platform.hpp"

namespace ghidraengine {
namespace {

using Bytes = std::span<const std::uint8_t>;

template <std::size_t N>
bool starts_with(Bytes data, const char (&magic)[N]) noexcept {
    constexpr std::size_t length = N - 1;
    return data.size() >= length && std::memcmp(data.data(), magic, length) == 0;
}

template <std::size_t N>
bool bytes_at(Bytes data, std::size_t offset, const char (&magic)[N]) noexcept {
    constexpr std::size_t length = N - 1;
    return data.size() >= offset + length &&
           std::memcmp(data.data() + offset, magic, length) == 0;
}

MediaKind classify_iso_bmff(Bytes data) noexcept {
    if (!bytes_at(data, 4, "ftyp")) {
        return MediaKind::Unknown;
    }
    if (data.size() < 12) {
        return MediaKind::Unknown;
    }
    const std::string_view brand(reinterpret_cast<const char*>(data.data()) + 8, 4);

    static constexpr std::array kImageBrands = {
        "heic", "heix", "heim", "heis", "hevc", "hevx", "mif1", "msf1",
        "avif", "avis", "jxl ",
    };
    for (const auto* candidate : kImageBrands) {
        if (brand == std::string_view(candidate, 4)) {
            return MediaKind::Image;
        }
    }

    return MediaKind::Video;
}

bool is_mpeg_transport_stream(Bytes data) noexcept {
    if (data.size() < 189 || data[0] != 0x47) {
        return false;
    }
    if (data.size() >= 377) {
        return data[188] == 0x47 && data[376] == 0x47;
    }
    return data[188] == 0x47;
}

}

MediaKind classify_header(Bytes data) noexcept {
    if (data.size() < 4) {
        return MediaKind::Unknown;
    }

    if (starts_with(data, "\xFF\xD8\xFF")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "\x89PNG\r\n\x1A\n")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "GIF87a") || starts_with(data, "GIF89a")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "BM")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "8BPS")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "qoif")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "\xFF\x0A") ||
        starts_with(data, "\x00\x00\x00\x0CJXL \r\n\x87\n")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "FUJIFILMCCD-RAW")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "FOVb")) {
        return MediaKind::Image;
    }
    if (starts_with(data, "II\x2A\x00") || starts_with(data, "MM\x00\x2A") ||
        starts_with(data, "II\x2B\x00") || starts_with(data, "MM\x00\x2B") ||
        starts_with(data, "IIRO") || starts_with(data, "IIU\x00")) {
        return MediaKind::Image;
    }

    if (starts_with(data, "RIFF") && data.size() >= 12) {
        if (bytes_at(data, 8, "WEBP")) {
            return MediaKind::Image;
        }
        if (bytes_at(data, 8, "AVI ")) {
            return MediaKind::Video;
        }
        return MediaKind::Unknown;
    }

    if (const MediaKind kind = classify_iso_bmff(data); kind != MediaKind::Unknown) {
        return kind;
    }

    if (starts_with(data, "\x1A\x45\xDF\xA3")) {
        return MediaKind::Video;
    }
    if (starts_with(data, "FLV\x01")) {
        return MediaKind::Video;
    }
    if (starts_with(data, "\x30\x26\xB2\x75\x8E\x66\xCF\x11")) {
        return MediaKind::Video;
    }
    if (starts_with(data, "OggS")) {
        return MediaKind::Video;
    }
    if (starts_with(data, "\x00\x00\x01\xBA") || starts_with(data, "\x00\x00\x01\xB3")) {
        return MediaKind::Video;
    }
    if (starts_with(data, "#!AMR")) {
        return MediaKind::Unknown;
    }
    if (is_mpeg_transport_stream(data)) {
        return MediaKind::Video;
    }

    return MediaKind::Unknown;
}

bool extension_is_candidate(const std::filesystem::path& path) noexcept {
    const std::filesystem::path extension = path.extension();
    if (extension.empty()) {
        return false;
    }

    std::string lower = platform::to_utf8(extension);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::unordered_set<std::string_view> kExtensions = {
        ".jpg", ".jpeg", ".jpe", ".jfif", ".png", ".gif", ".bmp", ".dib", ".webp",
        ".tif", ".tiff", ".heic", ".heif", ".hif", ".avif", ".jxl", ".psd", ".qoi",
        ".ico", ".tga", ".pbm", ".pgm", ".ppm", ".pnm", ".jp2", ".j2k", ".jpf",
        ".cr2", ".cr3", ".nef", ".nrw", ".arw", ".srf", ".sr2", ".dng", ".orf",
        ".pef", ".raf", ".rw2", ".raw", ".x3f", ".3fr", ".erf", ".mrw", ".kdc",
        ".mp4", ".m4v", ".mov", ".qt", ".mkv", ".webm", ".avi", ".wmv", ".asf",
        ".flv", ".f4v", ".mpg", ".mpeg", ".m2v", ".mts", ".m2ts", ".ts", ".vob",
        ".3gp", ".3g2", ".ogv", ".mxf", ".rm", ".rmvb", ".divx", ".m4s",
    };

    return kExtensions.contains(lower);
}

EnumerateResult enumerate_files(std::span<const std::filesystem::path> roots,
                                const ScanConfig& config,
                                const std::stop_token& token) {
    EnumerateResult result;

    std::unordered_set<FileIdentity, FileIdentityHash> visited_dirs;
    std::unordered_set<FileIdentity, FileIdentityHash> seen_files;

    struct Frame {
        std::filesystem::path path;
        std::uint32_t depth;
    };
    std::vector<Frame> stack;
    std::vector<platform::DirEntry> entries;

    const auto record_error = [&](const std::filesystem::path& path, Error error) {
        FileError failure{path, std::move(error)};
        if (config.on_error) {
            config.on_error(failure);
        }
        result.errors.push_back(std::move(failure));
    };

    const auto excluded = [&](const std::filesystem::path& path) {
        if (config.exclude_patterns.empty()) {
            return false;
        }
        const std::string text = platform::to_utf8(path);
        return std::any_of(config.exclude_patterns.begin(), config.exclude_patterns.end(),
                           [&](const std::string& pattern) {
                               return glob_match(pattern, text);
                           });
    };

    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || ec) {
            record_error(root, Error{ErrorCode::NotFound, "scan root does not exist"});
            continue;
        }
        if (!std::filesystem::is_directory(root, ec)) {
            platform::DirEntry entry;
            entry.path = root;
            if (auto stat = platform::stat_entry(root, entry); !stat) {
                record_error(root, stat.error());
                continue;
            }
            ++result.files_seen;
            if (entry.size >= config.min_file_size &&
                (config.max_file_size == 0 || entry.size <= config.max_file_size)) {
                FileEntry file;
                file.path = entry.path;
                file.size = entry.size;
                file.mtime_ns = entry.mtime_ns;
                if (auto identity = platform::identity_of(entry.path)) {
                    file.identity = identity.value();
                }
                result.files.push_back(std::move(file));
            }
            continue;
        }
        stack.push_back(Frame{root, 0});
    }

    while (!stack.empty()) {
        if (token.stop_possible() && token.stop_requested()) {
            result.cancelled = true;
            break;
        }

        const Frame frame = std::move(stack.back());
        stack.pop_back();

        if (auto listing = platform::list_directory(frame.path, entries); !listing) {
            record_error(frame.path, listing.error());
            continue;
        }

        for (auto& entry : entries) {
            ++result.files_seen;

            if (config.skip_hidden && entry.is_hidden) {
                continue;
            }
            if (entry.is_symlink && !config.follow_symlinks) {
                continue;
            }
            if (excluded(entry.path)) {
                continue;
            }

            if (entry.is_directory) {
                if (config.max_depth != 0 && frame.depth + 1 >= config.max_depth) {
                    continue;
                }
                if (entry.is_symlink) {
                    auto identity = platform::identity_of(entry.path);
                    if (!identity) {
                        record_error(entry.path, identity.error());
                        continue;
                    }
                    if (!visited_dirs.insert(identity.value()).second) {
                        continue;
                    }
                }
                stack.push_back(Frame{std::move(entry.path), frame.depth + 1});
                continue;
            }

            if (!entry.has_metadata) {
                if (auto stat = platform::stat_entry(entry.path, entry); !stat) {
                    record_error(entry.path, stat.error());
                    continue;
                }
            }

            if (entry.size < config.min_file_size) {
                continue;
            }
            if (config.max_file_size != 0 && entry.size > config.max_file_size) {
                continue;
            }
            if (!config.probe_unknown_extensions && !extension_is_candidate(entry.path)) {
                continue;
            }

            FileEntry file;
            file.path = std::move(entry.path);
            file.size = entry.size;
            file.mtime_ns = entry.mtime_ns;
            result.files.push_back(std::move(file));
        }

        if (config.on_progress) {
            config.on_progress(Progress{Progress::Phase::Enumerating, result.files.size(), 0});
        }
    }

    if (!result.files.empty()) {
        std::unordered_set<std::uint64_t> sizes;
        std::unordered_set<std::uint64_t> repeated;
        for (const auto& file : result.files) {
            if (!sizes.insert(file.size).second) {
                repeated.insert(file.size);
            }
        }

        std::vector<FileEntry> kept;
        kept.reserve(result.files.size());
        for (auto& file : result.files) {
            if (repeated.contains(file.size)) {
                if (auto identity = platform::identity_of(file.path)) {
                    file.identity = identity.value();
                    if (file.identity.valid() && !seen_files.insert(file.identity).second) {
                        ++result.hardlinks_collapsed;
                        continue;
                    }
                }
            }
            kept.push_back(std::move(file));
        }
        result.files = std::move(kept);
    }

    return result;
}

}
