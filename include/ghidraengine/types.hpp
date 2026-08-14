#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ghidraengine/error.hpp"

namespace ghidraengine {

inline constexpr std::size_t kMaxVideoFrames = 32;

inline constexpr std::size_t kThumbSize = 32;

inline constexpr std::size_t kColorMomentBytes = 32;

enum class MediaKind : std::uint8_t {
    Unknown = 0,
    Image,
    Video,
};

GHIDRAENGINE_API const char* to_string(MediaKind kind) noexcept;

enum class MatchKind : std::uint8_t {
    Exact = 0,
    Similar,
};

GHIDRAENGINE_API const char* to_string(MatchKind kind) noexcept;

struct Hash128 {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    friend bool operator==(const Hash128&, const Hash128&) = default;
};

using Hash256 = std::array<std::uint64_t, 4>;

struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t id_low = 0;
    std::uint64_t id_high = 0;

    [[nodiscard]] bool valid() const noexcept { return volume != 0 || id_low != 0 || id_high != 0; }
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

struct FileIdentityHash {
    GHIDRAENGINE_API std::size_t operator()(const FileIdentity& id) const noexcept;
};

struct FileEntry {
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    FileIdentity identity;
    MediaKind media = MediaKind::Unknown;
};

struct ImageSignature {
    std::uint64_t phash64 = 0;
    Hash256 phash256{};
    std::uint64_t dhash64 = 0;
    std::array<std::uint8_t, kColorMomentBytes> color{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool has_color = false;
};

struct VideoSignature {
    std::int64_t duration_ms = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_count = 0;
    std::array<std::uint64_t, kMaxVideoFrames> frames{};
    std::array<std::uint64_t, kMaxVideoFrames> sorted{};
};

struct VideoPreview {
    std::vector<std::uint8_t> rgb;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Signature {
    Hash128 partial_hash{};
    Hash128 full_hash{};
    ImageSignature image;
    VideoSignature video;
    bool has_partial_hash = false;
    bool has_full_hash = false;
    bool has_image = false;
    bool has_video = false;
};

struct Cluster {
    MatchKind kind = MatchKind::Exact;
    MediaKind media = MediaKind::Unknown;
    std::vector<std::uint32_t> members;
    std::uint32_t keeper = 0;
    std::vector<std::uint32_t> distances;
    std::uint64_t reclaimable_bytes = 0;
};

struct FileError {
    std::filesystem::path path;
    Error error;
};

struct ScanStats {
    std::uint64_t files_seen = 0;
    std::uint64_t files_considered = 0;
    std::uint64_t files_hashed = 0;
    std::uint64_t images_decoded = 0;
    std::uint64_t videos_probed = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t hardlinks_collapsed = 0;
    double elapsed_seconds = 0.0;
};

struct Report {
    std::vector<FileEntry> files;
    std::vector<Cluster> clusters;
    std::vector<FileError> errors;
    ScanStats stats;
    bool cancelled = false;

    [[nodiscard]] GHIDRAENGINE_API std::uint64_t total_reclaimable_bytes() const noexcept;
};

struct Progress {
    enum class Phase : std::uint8_t {
        Enumerating = 0,
        Hashing,
        Decoding,
        Indexing,
        Clustering,
        Done,
    };

    Phase phase = Phase::Enumerating;
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
};

GHIDRAENGINE_API const char* to_string(Progress::Phase phase) noexcept;

}
