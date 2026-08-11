#ifndef GHIDRAENGINE_C_H
#define GHIDRAENGINE_C_H

#include <stddef.h>
#include <stdint.h>

#include "ghidraengine/export.hpp"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ghidraengine_status {
    GHIDRAENGINE_OK = 0,
    GHIDRAENGINE_ERR_NOT_FOUND = 1,
    GHIDRAENGINE_ERR_ACCESS_DENIED = 2,
    GHIDRAENGINE_ERR_IO = 3,
    GHIDRAENGINE_ERR_UNSUPPORTED_FORMAT = 4,
    GHIDRAENGINE_ERR_DECODE_FAILED = 5,
    GHIDRAENGINE_ERR_CORRUPT_FILE = 6,
    GHIDRAENGINE_ERR_CANCELLED = 7,
    GHIDRAENGINE_ERR_CACHE = 8,
    GHIDRAENGINE_ERR_INVALID_ARGUMENT = 9,
    GHIDRAENGINE_ERR_OUT_OF_MEMORY = 10,
    GHIDRAENGINE_ERR_UNKNOWN = 11
} ghidraengine_status;

typedef enum ghidraengine_media_kind {
    GHIDRAENGINE_MEDIA_UNKNOWN = 0,
    GHIDRAENGINE_MEDIA_IMAGE = 1,
    GHIDRAENGINE_MEDIA_VIDEO = 2
} ghidraengine_media_kind;

typedef enum ghidraengine_match_kind {
    GHIDRAENGINE_MATCH_EXACT = 0,
    GHIDRAENGINE_MATCH_SIMILAR = 1
} ghidraengine_match_kind;

typedef enum ghidraengine_cluster_mode {
    GHIDRAENGINE_CLUSTER_STRICT = 0,
    GHIDRAENGINE_CLUSTER_TRANSITIVE = 1
} ghidraengine_cluster_mode;

typedef enum ghidraengine_keeper_policy {
    GHIDRAENGINE_KEEP_HIGHEST_RESOLUTION = 0,
    GHIDRAENGINE_KEEP_LARGEST_FILE = 1,
    GHIDRAENGINE_KEEP_OLDEST = 2,
    GHIDRAENGINE_KEEP_NEWEST = 3,
    GHIDRAENGINE_KEEP_SHORTEST_PATH = 4
} ghidraengine_keeper_policy;

typedef enum ghidraengine_phase {
    GHIDRAENGINE_PHASE_ENUMERATING = 0,
    GHIDRAENGINE_PHASE_HASHING = 1,
    GHIDRAENGINE_PHASE_DECODING = 2,
    GHIDRAENGINE_PHASE_INDEXING = 3,
    GHIDRAENGINE_PHASE_CLUSTERING = 4,
    GHIDRAENGINE_PHASE_DONE = 5
} ghidraengine_phase;

typedef struct ghidraengine_config {
    int detect_exact;
    int detect_similar;
    int scan_images;
    int scan_videos;

    uint64_t min_file_size;
    uint64_t max_file_size;
    int follow_symlinks;
    int skip_hidden;
    uint32_t max_depth;

    uint32_t phash_threshold;
    uint32_t phash256_threshold;
    uint32_t dhash_threshold;
    uint32_t color_threshold;
    int dihedral_invariant;
    uint32_t min_dimension;

    uint32_t video_frame_samples;
    double video_edge_skip_fraction;
    double video_duration_tolerance;
    uint32_t video_frame_threshold;
    double video_min_frame_match_ratio;
    double video_min_frame_variance;
    int video_subclip_detection;

    int verify_bytes;
    int cluster_mode;
    int keeper_policy;

    uint32_t cpu_threads;
    uint32_t io_threads;

    int cache_enabled;
    const char* cache_path;
    uint32_t cache_prune_after_days;
} ghidraengine_config;

typedef struct ghidraengine_scanner ghidraengine_scanner;
typedef struct ghidraengine_report ghidraengine_report;

typedef int (*ghidraengine_progress_fn)(int phase, uint64_t processed, uint64_t total, void* user_data);

GHIDRAENGINE_API const char* ghidraengine_version_string(void);
GHIDRAENGINE_API const char* ghidraengine_status_message(ghidraengine_status status);
GHIDRAENGINE_API const char* ghidraengine_simd_backend(void);

GHIDRAENGINE_API void ghidraengine_config_init(ghidraengine_config* config);

GHIDRAENGINE_API ghidraengine_status ghidraengine_scanner_create(const ghidraengine_config* config,
                                                  ghidraengine_scanner** out_scanner);
GHIDRAENGINE_API void ghidraengine_scanner_free(ghidraengine_scanner* scanner);

GHIDRAENGINE_API void ghidraengine_scanner_set_progress(ghidraengine_scanner* scanner,
                                              ghidraengine_progress_fn callback,
                                              void* user_data);

GHIDRAENGINE_API void ghidraengine_scanner_cancel(ghidraengine_scanner* scanner);

GHIDRAENGINE_API ghidraengine_status ghidraengine_scanner_scan(ghidraengine_scanner* scanner,
                                                const char* const* roots,
                                                size_t root_count,
                                                ghidraengine_report** out_report);

GHIDRAENGINE_API const char* ghidraengine_scanner_last_error(const ghidraengine_scanner* scanner);

GHIDRAENGINE_API void ghidraengine_report_free(ghidraengine_report* report);
GHIDRAENGINE_API int ghidraengine_report_was_cancelled(const ghidraengine_report* report);

GHIDRAENGINE_API size_t ghidraengine_report_file_count(const ghidraengine_report* report);
GHIDRAENGINE_API const char* ghidraengine_report_file_path(const ghidraengine_report* report, size_t index);
GHIDRAENGINE_API uint64_t ghidraengine_report_file_size(const ghidraengine_report* report, size_t index);
GHIDRAENGINE_API int64_t ghidraengine_report_file_mtime_ns(const ghidraengine_report* report, size_t index);
GHIDRAENGINE_API int ghidraengine_report_file_media_kind(const ghidraengine_report* report, size_t index);

GHIDRAENGINE_API size_t ghidraengine_report_cluster_count(const ghidraengine_report* report);
GHIDRAENGINE_API int ghidraengine_report_cluster_match_kind(const ghidraengine_report* report, size_t cluster);
GHIDRAENGINE_API int ghidraengine_report_cluster_media_kind(const ghidraengine_report* report, size_t cluster);
GHIDRAENGINE_API size_t ghidraengine_report_cluster_member_count(const ghidraengine_report* report, size_t cluster);
GHIDRAENGINE_API uint32_t ghidraengine_report_cluster_member(const ghidraengine_report* report,
                                                   size_t cluster, size_t member);
GHIDRAENGINE_API uint32_t ghidraengine_report_cluster_member_distance(const ghidraengine_report* report,
                                                            size_t cluster, size_t member);
GHIDRAENGINE_API uint32_t ghidraengine_report_cluster_keeper(const ghidraengine_report* report, size_t cluster);
GHIDRAENGINE_API uint64_t ghidraengine_report_cluster_reclaimable(const ghidraengine_report* report, size_t cluster);

GHIDRAENGINE_API size_t ghidraengine_report_error_count(const ghidraengine_report* report);
GHIDRAENGINE_API const char* ghidraengine_report_error_path(const ghidraengine_report* report, size_t index);
GHIDRAENGINE_API const char* ghidraengine_report_error_message(const ghidraengine_report* report, size_t index);
GHIDRAENGINE_API int ghidraengine_report_error_code(const ghidraengine_report* report, size_t index);

GHIDRAENGINE_API uint64_t ghidraengine_report_total_reclaimable(const ghidraengine_report* report);

GHIDRAENGINE_API void ghidraengine_report_stats(const ghidraengine_report* report,
                                      uint64_t* files_seen,
                                      uint64_t* files_considered,
                                      uint64_t* files_hashed,
                                      uint64_t* images_decoded,
                                      uint64_t* videos_probed,
                                      uint64_t* cache_hits,
                                      uint64_t* bytes_read,
                                      double* elapsed_seconds);

#ifdef __cplusplus
}
#endif

#endif
