#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ghidraengine/error.hpp"

namespace ghidraengine {

// Fixed so a VideoSignature stays a flat value with no per-file heap allocation.
inline constexpr std::size_t kMaxVideoFrames = 32;

// Perceptual hashes are computed from a fixed 32x32 grayscale buffer.
inline constexpr std::size_t kThumbSize = 32;

// 4x4 grid x 2 chroma channels, one byte each.
inline constexpr std::size_t kColorMomentBytes = 32;

enum class MediaKind : std::uint8_t {
    Unknown = 0,
    Image,
    Video,
};

const char* to_string(MediaKind kind) noexcept;

enum class MatchKind : std::uint8_t {
    Exact = 0, // byte-for-byte identical content
    Similar,   // perceptually equivalent (re-encoded, resized, recompressed)
};

const char* to_string(MatchKind kind) noexcept;

struct Hash128 {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    friend bool operator==(const Hash128&, const Hash128&) = default;
};

using Hash256 = std::array<std::uint64_t, 4>;

// Identity of the physical file, independent of the path used to reach it.
// Windows: volume serial + 128-bit file id. POSIX: st_dev + st_ino.
// Two entries sharing an identity are hard links to one extent, not duplicates.
struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t id_low = 0;
    std::uint64_t id_high = 0;

    [[nodiscard]] bool valid() const noexcept { return volume != 0 || id_low != 0 || id_high != 0; }
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

struct FileIdentityHash {
    std::size_t operator()(const FileIdentity& id) const noexcept;
};

struct FileEntry {
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0; // nanoseconds since the Unix epoch
    FileIdentity identity;
    MediaKind media = MediaKind::Unknown;
};

// All four hashes come from one decode: decoding dominates the cost, so extra
// hashes are nearly free and each covers a different failure mode of the others.
struct ImageSignature {
    std::uint64_t phash64 = 0;  // DCT of 32x32, top-left 8x8 minus DC, median threshold
    Hash256 phash256{};         // same DCT, top-left 16x16
    std::uint64_t dhash64 = 0;  // horizontal gradient
    std::array<std::uint8_t, kColorMomentBytes> color{}; // 4x4 grid means of chroma
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool has_color = false; // false for grayscale sources; disables the chroma check
};

struct VideoSignature {
    std::int64_t duration_ms = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_count = 0;
    std::array<std::uint64_t, kMaxVideoFrames> frames{}; // timeline order
    std::array<std::uint64_t, kMaxVideoFrames> sorted{}; // for order-independent merge
};

// Cheap fields are filled first, expensive ones only if an earlier stage failed
// to resolve the file.
struct Signature {
    Hash128 partial_hash{}; // head + tail, for size-collision groups only
    Hash128 full_hash{};    // whole file, only when partial hashes collided
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
    // Indices into Report::files. Always at least two entries.
    std::vector<std::uint32_t> members;
    std::uint32_t keeper = 0;
    // Distance from each member to the keeper, parallel to `members`: Hamming bits
    // for Similar clusters, always 0 for Exact ones.
    std::vector<std::uint32_t> distances;
    // Bytes reclaimed by keeping only `keeper`.
    std::uint64_t reclaimable_bytes = 0;
};

struct FileError {
    std::filesystem::path path;
    Error error;
};

struct ScanStats {
    std::uint64_t files_seen = 0;        // entries the walker visited
    std::uint64_t files_considered = 0;  // survived filters, entered the pipeline
    std::uint64_t files_hashed = 0;
    std::uint64_t images_decoded = 0;
    std::uint64_t videos_probed = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t hardlinks_collapsed = 0; // same identity reached via several paths
    double elapsed_seconds = 0.0;
};

struct Report {
    std::vector<FileEntry> files;
    std::vector<Cluster> clusters;
    std::vector<FileError> errors;
    ScanStats stats;
    bool cancelled = false;

    [[nodiscard]] std::uint64_t total_reclaimable_bytes() const noexcept;
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
    std::uint64_t total = 0; // 0 while the total is still unknown
};

const char* to_string(Progress::Phase phase) noexcept;

} // namespace ghidraengine
