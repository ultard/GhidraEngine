#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ghidraengine/types.hpp"

namespace ghidraengine {

enum class ClusterMode : std::uint8_t {
    // A file joins a cluster only if it matches that cluster's representative,
    // which stops A~B, B~C, A!~C chains from merging unrelated images.
    Strict = 0,
    // Transitive closure via union-find: higher recall, but chains.
    Transitive,
};

// Which member of a cluster the report recommends keeping. Ties fall through to
// the next criterion, ending with the shortest path for determinism.
enum class KeeperPolicy : std::uint8_t {
    HighestResolution = 0,
    LargestFile,
    OldestModified,
    NewestModified,
    ShortestPath,
};

struct ImageMatchConfig {
    // 10/64 is the sweet spot: 8 misses mild crops, 14 starts pulling in
    // unrelated images with similar composition.
    std::uint32_t phash_threshold = 10;

    // Confirmation on the 256-bit hash; a candidate must pass both. This is what
    // keeps precision high on corpora above ~100k images.
    std::uint32_t phash256_threshold = 40;

    // Gradient hash confirmation. 64 disables it.
    std::uint32_t dhash_threshold = 16;

    // Maximum mean absolute chroma difference (0-255). Rejects the classic DCT
    // false positive: different pictures that agree in grayscale. Only applied
    // when both images carry colour; 255 disables the check.
    std::uint32_t color_threshold = 24;

    // Rotate the thumbnail to a canonical orientation before hashing, so the 8
    // rotations and mirrors of one picture all hash the same.
    bool dihedral_invariant = false;

    // Images smaller than this on either axis carry too little signal.
    std::uint32_t min_dimension = 32;
};

struct VideoMatchConfig {
    // Clamped to kMaxVideoFrames.
    std::uint32_t frame_samples = 16;

    // Fraction of the timeline skipped at each end, avoiding intros and credits.
    double edge_skip_fraction = 0.05;

    // Cheap prefilter: durations must agree within this fraction to be compared.
    double duration_tolerance = 0.02;

    std::uint32_t frame_threshold = 8;
    double min_frame_match_ratio = 0.65;

    // Frames below this variance are uniform (black, fades) and hash to something
    // that matches everything. They are resampled nearby instead.
    double min_frame_variance = 12.0;

    // Detect one video contained within a longer one. Off by default: quadratic
    // in samples per candidate pair.
    bool subclip_detection = false;
};

struct ConcurrencyConfig {
    // 0 means "decide from hardware_concurrency()".
    std::uint32_t cpu_threads = 0;

    // Throttled independently of cpu_threads: on flash, parallel reads are a large
    // win, while on rotational media they turn one sequential stream into a seek
    // storm. 0 auto-detects from the storage backing the first scan root.
    std::uint32_t io_threads = 0;
};

struct CacheConfig {
    // Off by default: a scan writes nothing outside the scanned tree unless asked.
    bool enabled = false;

    // Empty means "ghidraengine-cache.db" in the first scan root. Nothing is ever
    // written to a user-wide directory.
    std::filesystem::path path;

    std::uint32_t prune_after_days = 90;
};

struct ScanConfig {
    bool detect_exact = true;
    bool detect_similar = true;
    bool scan_images = true;
    bool scan_videos = true;

    std::uint64_t min_file_size = 4096;
    std::uint64_t max_file_size = 0;         // 0 = unlimited
    bool follow_symlinks = false;
    bool skip_hidden = true;
    std::uint32_t max_depth = 0;             // 0 = unlimited

    // Globs matched against the full path; matches are skipped.
    std::vector<std::string> exclude_patterns;

    // Media type is always decided by the file's leading bytes. The extension only
    // decides which files are worth opening at all; this opens every file, so a
    // JPEG named ".bak" is still found.
    bool probe_unknown_extensions = false;

    ImageMatchConfig image;
    VideoMatchConfig video;

    // The 128-bit content hash makes a collision astronomically unlikely, so the
    // full byte comparison is opt-in.
    bool verify_bytes = false;

    ClusterMode cluster_mode = ClusterMode::Strict;
    KeeperPolicy keeper_policy = KeeperPolicy::HighestResolution;

    ConcurrencyConfig concurrency;
    CacheConfig cache;

    // Both are invoked from worker threads: must be thread-safe and must not block.
    std::function<void(const Progress&)> on_progress;
    std::function<void(const FileError&)> on_error;

    [[nodiscard]] Result<void> validate() const;
};

} // namespace ghidraengine
