#include "ghidraengine/ghidraengine_c.h"

#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stop_token>
#include <string>
#include <vector>

#include "ghidraengine/ghidraengine.hpp"
#include "core/platform.hpp"

namespace {

using namespace ghidraengine;

ghidraengine_status to_status(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return GHIDRAENGINE_OK;
        case ErrorCode::NotFound: return GHIDRAENGINE_ERR_NOT_FOUND;
        case ErrorCode::AccessDenied: return GHIDRAENGINE_ERR_ACCESS_DENIED;
        case ErrorCode::IoError: return GHIDRAENGINE_ERR_IO;
        case ErrorCode::UnsupportedFormat: return GHIDRAENGINE_ERR_UNSUPPORTED_FORMAT;
        case ErrorCode::DecodeFailed: return GHIDRAENGINE_ERR_DECODE_FAILED;
        case ErrorCode::CorruptFile: return GHIDRAENGINE_ERR_CORRUPT_FILE;
        case ErrorCode::Cancelled: return GHIDRAENGINE_ERR_CANCELLED;
        case ErrorCode::CacheError: return GHIDRAENGINE_ERR_CACHE;
        case ErrorCode::InvalidArgument: return GHIDRAENGINE_ERR_INVALID_ARGUMENT;
        case ErrorCode::OutOfMemory: return GHIDRAENGINE_ERR_OUT_OF_MEMORY;
        case ErrorCode::Unknown: break;
    }
    return GHIDRAENGINE_ERR_UNKNOWN;
}

ScanConfig from_c(const ghidraengine_config& source) {
    ScanConfig config;
    config.detect_exact = source.detect_exact != 0;
    config.detect_similar = source.detect_similar != 0;
    config.scan_images = source.scan_images != 0;
    config.scan_videos = source.scan_videos != 0;

    config.min_file_size = source.min_file_size;
    config.max_file_size = source.max_file_size;
    config.follow_symlinks = source.follow_symlinks != 0;
    config.skip_hidden = source.skip_hidden != 0;
    config.max_depth = source.max_depth;

    config.image.phash_threshold = source.phash_threshold;
    config.image.phash256_threshold = source.phash256_threshold;
    config.image.dhash_threshold = source.dhash_threshold;
    config.image.color_threshold = source.color_threshold;
    config.image.dihedral_invariant = source.dihedral_invariant != 0;
    config.image.min_dimension = source.min_dimension;

    config.video.frame_samples = source.video_frame_samples;
    config.video.edge_skip_fraction = source.video_edge_skip_fraction;
    config.video.duration_tolerance = source.video_duration_tolerance;
    config.video.frame_threshold = source.video_frame_threshold;
    config.video.min_frame_match_ratio = source.video_min_frame_match_ratio;
    config.video.min_frame_variance = source.video_min_frame_variance;
    config.video.subclip_detection = source.video_subclip_detection != 0;

    config.verify_bytes = source.verify_bytes != 0;
    config.cluster_mode = source.cluster_mode == GHIDRAENGINE_CLUSTER_TRANSITIVE
                              ? ClusterMode::Transitive
                              : ClusterMode::Strict;
    config.keeper_policy = static_cast<KeeperPolicy>(source.keeper_policy);

    config.concurrency.cpu_threads = source.cpu_threads;
    config.concurrency.io_threads = source.io_threads;

    config.cache.enabled = source.cache_enabled != 0;
    if (source.cache_path != nullptr && *source.cache_path != '\0') {
        config.cache.path = platform::from_utf8(source.cache_path);
    }
    config.cache.prune_after_days = source.cache_prune_after_days;

    return config;
}

} // namespace

struct ghidraengine_report {
    Report report;
    std::vector<std::string> file_paths;
    std::vector<std::string> error_paths;
    std::vector<std::string> error_messages;
};

struct ghidraengine_scanner {
    ScanConfig config;
    std::string last_error;
    ghidraengine_progress_fn progress = nullptr;
    void* progress_user_data = nullptr;
    std::stop_source stop;
    std::mutex mutex;
};

extern "C" {

const char* ghidraengine_version_string(void) { return ghidraengine::version_string(); }

const char* ghidraengine_simd_backend(void) { return ghidraengine::active_simd_backend(); }

const char* ghidraengine_status_message(ghidraengine_status status) {
    switch (status) {
        case GHIDRAENGINE_OK: return "ok";
        case GHIDRAENGINE_ERR_NOT_FOUND: return "file not found";
        case GHIDRAENGINE_ERR_ACCESS_DENIED: return "access denied";
        case GHIDRAENGINE_ERR_IO: return "I/O error";
        case GHIDRAENGINE_ERR_UNSUPPORTED_FORMAT: return "unsupported format";
        case GHIDRAENGINE_ERR_DECODE_FAILED: return "decode failed";
        case GHIDRAENGINE_ERR_CORRUPT_FILE: return "corrupt or truncated file";
        case GHIDRAENGINE_ERR_CANCELLED: return "cancelled";
        case GHIDRAENGINE_ERR_CACHE: return "cache error";
        case GHIDRAENGINE_ERR_INVALID_ARGUMENT: return "invalid argument";
        case GHIDRAENGINE_ERR_OUT_OF_MEMORY: return "out of memory";
        case GHIDRAENGINE_ERR_UNKNOWN: break;
    }
    return "unknown error";
}

void ghidraengine_config_init(ghidraengine_config* config) {
    if (config == nullptr) {
        return;
    }
    // Mirrors the C++ defaults rather than restating them, so they cannot drift.
    const ScanConfig defaults;

    config->detect_exact = defaults.detect_exact ? 1 : 0;
    config->detect_similar = defaults.detect_similar ? 1 : 0;
    config->scan_images = defaults.scan_images ? 1 : 0;
    config->scan_videos = defaults.scan_videos ? 1 : 0;

    config->min_file_size = defaults.min_file_size;
    config->max_file_size = defaults.max_file_size;
    config->follow_symlinks = defaults.follow_symlinks ? 1 : 0;
    config->skip_hidden = defaults.skip_hidden ? 1 : 0;
    config->max_depth = defaults.max_depth;

    config->phash_threshold = defaults.image.phash_threshold;
    config->phash256_threshold = defaults.image.phash256_threshold;
    config->dhash_threshold = defaults.image.dhash_threshold;
    config->color_threshold = defaults.image.color_threshold;
    config->dihedral_invariant = defaults.image.dihedral_invariant ? 1 : 0;
    config->min_dimension = defaults.image.min_dimension;

    config->video_frame_samples = defaults.video.frame_samples;
    config->video_edge_skip_fraction = defaults.video.edge_skip_fraction;
    config->video_duration_tolerance = defaults.video.duration_tolerance;
    config->video_frame_threshold = defaults.video.frame_threshold;
    config->video_min_frame_match_ratio = defaults.video.min_frame_match_ratio;
    config->video_min_frame_variance = defaults.video.min_frame_variance;
    config->video_subclip_detection = defaults.video.subclip_detection ? 1 : 0;

    config->verify_bytes = defaults.verify_bytes ? 1 : 0;
    config->cluster_mode = static_cast<int>(defaults.cluster_mode);
    config->keeper_policy = static_cast<int>(defaults.keeper_policy);

    config->cpu_threads = defaults.concurrency.cpu_threads;
    config->io_threads = defaults.concurrency.io_threads;

    config->cache_enabled = defaults.cache.enabled ? 1 : 0;
    config->cache_path = nullptr;
    config->cache_prune_after_days = defaults.cache.prune_after_days;
}

ghidraengine_status ghidraengine_scanner_create(const ghidraengine_config* config,
                                      ghidraengine_scanner** out_scanner) {
    if (out_scanner == nullptr) {
        return GHIDRAENGINE_ERR_INVALID_ARGUMENT;
    }
    *out_scanner = nullptr;

    try {
        auto scanner = std::make_unique<ghidraengine_scanner>();
        if (config != nullptr) {
            scanner->config = from_c(*config);
        }
        if (auto valid = scanner->config.validate(); !valid) {
            return to_status(valid.error().code);
        }
        *out_scanner = scanner.release();
        return GHIDRAENGINE_OK;
    } catch (const std::bad_alloc&) {
        return GHIDRAENGINE_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return GHIDRAENGINE_ERR_UNKNOWN;
    }
}

void ghidraengine_scanner_free(ghidraengine_scanner* scanner) { delete scanner; }

void ghidraengine_scanner_set_progress(ghidraengine_scanner* scanner, ghidraengine_progress_fn callback,
                                  void* user_data) {
    if (scanner == nullptr) {
        return;
    }
    const std::lock_guard lock(scanner->mutex);
    scanner->progress = callback;
    scanner->progress_user_data = user_data;
}

void ghidraengine_scanner_cancel(ghidraengine_scanner* scanner) {
    if (scanner == nullptr) {
        return;
    }
    scanner->stop.request_stop();
}

const char* ghidraengine_scanner_last_error(const ghidraengine_scanner* scanner) {
    return scanner != nullptr ? scanner->last_error.c_str() : "";
}

ghidraengine_status ghidraengine_scanner_scan(ghidraengine_scanner* scanner, const char* const* roots,
                                    size_t root_count, ghidraengine_report** out_report) {
    if (scanner == nullptr || out_report == nullptr || (roots == nullptr && root_count > 0)) {
        return GHIDRAENGINE_ERR_INVALID_ARGUMENT;
    }
    *out_report = nullptr;

    try {
        scanner->last_error.clear();

        std::vector<std::filesystem::path> paths;
        paths.reserve(root_count);
        for (size_t i = 0; i < root_count; ++i) {
            if (roots[i] == nullptr) {
                return GHIDRAENGINE_ERR_INVALID_ARGUMENT;
            }
            paths.push_back(platform::from_utf8(roots[i]));
        }

        ScanConfig config = scanner->config;
        if (scanner->progress != nullptr) {
            // A non-zero return is the only way a C caller can stop a scan from
            // inside the callback.
            config.on_progress = [scanner](const Progress& progress) {
                if (scanner->progress(static_cast<int>(progress.phase), progress.processed,
                                      progress.total, scanner->progress_user_data) != 0) {
                    scanner->stop.request_stop();
                }
            };
        }

        // Fresh per scan, so a previous cancellation does not abort this one.
        scanner->stop = std::stop_source{};

        Scanner engine(std::move(config));
        auto result = engine.scan(paths, scanner->stop.get_token());
        if (!result) {
            scanner->last_error = result.error().message;
            return to_status(result.error().code);
        }

        auto wrapper = std::make_unique<ghidraengine_report>();
        wrapper->report = std::move(result.value());

        wrapper->file_paths.reserve(wrapper->report.files.size());
        for (const auto& file : wrapper->report.files) {
            wrapper->file_paths.push_back(platform::to_utf8(file.path));
        }
        wrapper->error_paths.reserve(wrapper->report.errors.size());
        wrapper->error_messages.reserve(wrapper->report.errors.size());
        for (const auto& error : wrapper->report.errors) {
            wrapper->error_paths.push_back(platform::to_utf8(error.path));
            wrapper->error_messages.push_back(error.error.message);
        }

        *out_report = wrapper.release();
        return GHIDRAENGINE_OK;
    } catch (const std::bad_alloc&) {
        return GHIDRAENGINE_ERR_OUT_OF_MEMORY;
    } catch (const std::exception& error) {
        scanner->last_error = error.what();
        return GHIDRAENGINE_ERR_UNKNOWN;
    } catch (...) {
        return GHIDRAENGINE_ERR_UNKNOWN;
    }
}

// --- Report accessors ----------------------------------------------------

void ghidraengine_report_free(ghidraengine_report* report) { delete report; }

int ghidraengine_report_was_cancelled(const ghidraengine_report* report) {
    return (report != nullptr && report->report.cancelled) ? 1 : 0;
}

size_t ghidraengine_report_file_count(const ghidraengine_report* report) {
    return report != nullptr ? report->report.files.size() : 0;
}

const char* ghidraengine_report_file_path(const ghidraengine_report* report, size_t index) {
    if (report == nullptr || index >= report->file_paths.size()) {
        return "";
    }
    return report->file_paths[index].c_str();
}

uint64_t ghidraengine_report_file_size(const ghidraengine_report* report, size_t index) {
    if (report == nullptr || index >= report->report.files.size()) {
        return 0;
    }
    return report->report.files[index].size;
}

int64_t ghidraengine_report_file_mtime_ns(const ghidraengine_report* report, size_t index) {
    if (report == nullptr || index >= report->report.files.size()) {
        return 0;
    }
    return report->report.files[index].mtime_ns;
}

int ghidraengine_report_file_media_kind(const ghidraengine_report* report, size_t index) {
    if (report == nullptr || index >= report->report.files.size()) {
        return GHIDRAENGINE_MEDIA_UNKNOWN;
    }
    return static_cast<int>(report->report.files[index].media);
}

size_t ghidraengine_report_cluster_count(const ghidraengine_report* report) {
    return report != nullptr ? report->report.clusters.size() : 0;
}

int ghidraengine_report_cluster_match_kind(const ghidraengine_report* report, size_t cluster) {
    if (report == nullptr || cluster >= report->report.clusters.size()) {
        return GHIDRAENGINE_MATCH_EXACT;
    }
    return static_cast<int>(report->report.clusters[cluster].kind);
}

int ghidraengine_report_cluster_media_kind(const ghidraengine_report* report, size_t cluster) {
    if (report == nullptr || cluster >= report->report.clusters.size()) {
        return GHIDRAENGINE_MEDIA_UNKNOWN;
    }
    return static_cast<int>(report->report.clusters[cluster].media);
}

size_t ghidraengine_report_cluster_member_count(const ghidraengine_report* report, size_t cluster) {
    if (report == nullptr || cluster >= report->report.clusters.size()) {
        return 0;
    }
    return report->report.clusters[cluster].members.size();
}

uint32_t ghidraengine_report_cluster_member(const ghidraengine_report* report, size_t cluster,
                                       size_t member) {
    if (report == nullptr || cluster >= report->report.clusters.size()) {
        return 0;
    }
    const auto& members = report->report.clusters[cluster].members;
    return member < members.size() ? members[member] : 0;
}

uint32_t ghidraengine_report_cluster_member_distance(const ghidraengine_report* report, size_t cluster,
                                                size_t member) {
    if (report == nullptr || cluster >= report->report.clusters.size()) {
        return 0;
    }
    const auto& distances = report->report.clusters[cluster].distances;
    return member < distances.size() ? distances[member] : 0;
}

uint32_t ghidraengine_report_cluster_keeper(const ghidraengine_report* report, size_t cluster) {
    if (report == nullptr || cluster >= report->report.clusters.size()) {
        return 0;
    }
    return report->report.clusters[cluster].keeper;
}

uint64_t ghidraengine_report_cluster_reclaimable(const ghidraengine_report* report, size_t cluster) {
    if (report == nullptr || cluster >= report->report.clusters.size()) {
        return 0;
    }
    return report->report.clusters[cluster].reclaimable_bytes;
}

size_t ghidraengine_report_error_count(const ghidraengine_report* report) {
    return report != nullptr ? report->report.errors.size() : 0;
}

const char* ghidraengine_report_error_path(const ghidraengine_report* report, size_t index) {
    if (report == nullptr || index >= report->error_paths.size()) {
        return "";
    }
    return report->error_paths[index].c_str();
}

const char* ghidraengine_report_error_message(const ghidraengine_report* report, size_t index) {
    if (report == nullptr || index >= report->error_messages.size()) {
        return "";
    }
    return report->error_messages[index].c_str();
}

int ghidraengine_report_error_code(const ghidraengine_report* report, size_t index) {
    if (report == nullptr || index >= report->report.errors.size()) {
        return GHIDRAENGINE_ERR_UNKNOWN;
    }
    return to_status(report->report.errors[index].error.code);
}

uint64_t ghidraengine_report_total_reclaimable(const ghidraengine_report* report) {
    return report != nullptr ? report->report.total_reclaimable_bytes() : 0;
}

void ghidraengine_report_stats(const ghidraengine_report* report, uint64_t* files_seen,
                          uint64_t* files_considered, uint64_t* files_hashed,
                          uint64_t* images_decoded, uint64_t* videos_probed,
                          uint64_t* cache_hits, uint64_t* bytes_read,
                          double* elapsed_seconds) {
    if (report == nullptr) {
        return;
    }
    const ScanStats& stats = report->report.stats;
    if (files_seen != nullptr) *files_seen = stats.files_seen;
    if (files_considered != nullptr) *files_considered = stats.files_considered;
    if (files_hashed != nullptr) *files_hashed = stats.files_hashed;
    if (images_decoded != nullptr) *images_decoded = stats.images_decoded;
    if (videos_probed != nullptr) *videos_probed = stats.videos_probed;
    if (cache_hits != nullptr) *cache_hits = stats.cache_hits;
    if (bytes_read != nullptr) *bytes_read = stats.bytes_read;
    if (elapsed_seconds != nullptr) *elapsed_seconds = stats.elapsed_seconds;
}

} // extern "C"
