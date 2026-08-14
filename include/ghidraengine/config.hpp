#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ghidraengine/types.hpp"

namespace ghidraengine {

enum class ClusterMode : std::uint8_t {
    Strict = 0,
    Transitive,
};

enum class KeeperPolicy : std::uint8_t {
    HighestResolution = 0,
    LargestFile,
    OldestModified,
    NewestModified,
    ShortestPath,
};

struct ImageMatchConfig {
    std::uint32_t phash_threshold = 10;

    std::uint32_t phash256_threshold = 40;

    std::uint32_t dhash_threshold = 16;

    std::uint32_t color_threshold = 24;

    bool dihedral_invariant = false;

    std::uint32_t min_dimension = 32;
};

struct VideoMatchConfig {
    std::uint32_t frame_samples = 16;

    double edge_skip_fraction = 0.05;

    double duration_tolerance = 0.02;

    std::uint32_t frame_threshold = 8;
    double min_frame_match_ratio = 0.65;

    double min_frame_variance = 12.0;

    bool subclip_detection = false;
};

struct ConcurrencyConfig {
    std::uint32_t cpu_threads = 0;

    std::uint32_t io_threads = 0;
};

struct CacheConfig {
    bool enabled = false;

    std::filesystem::path path;

    std::uint32_t prune_after_days = 90;
};

struct ScanConfig {
    bool detect_exact = true;
    bool detect_similar = true;
    bool scan_images = true;
    bool scan_videos = true;

    std::uint64_t min_file_size = 4096;
    std::uint64_t max_file_size = 0;
    bool follow_symlinks = false;
    bool skip_hidden = true;
    std::uint32_t max_depth = 0;

    std::vector<std::string> exclude_patterns;

    bool probe_unknown_extensions = false;

    ImageMatchConfig image;
    VideoMatchConfig video;

    bool verify_bytes = false;

    ClusterMode cluster_mode = ClusterMode::Strict;
    KeeperPolicy keeper_policy = KeeperPolicy::HighestResolution;

    ConcurrencyConfig concurrency;
    CacheConfig cache;

    // Both are invoked from worker threads: must be thread-safe and must not block.
    std::function<void(const Progress&)> on_progress;
    std::function<void(const FileError&)> on_error;

    [[nodiscard]] GHIDRAENGINE_API Result<void> validate() const;
};

}
