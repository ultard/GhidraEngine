#ifndef GHIDRAENGINE_SCAN_SCANNER_HPP
#define GHIDRAENGINE_SCAN_SCANNER_HPP

#include <GhidraEngine/cache/fingerprint.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace GhidraEngine {

struct ScanOptions {
    FingerprintSettings fingerprints;
    std::size_t workers = 1;
    std::size_t max_files = 1'000'000;
};

struct ScannedFile {
    MediaId id;
    std::filesystem::path path;
    std::uintmax_t size{};
    std::optional<Blake3Digest> digest;
    std::optional<MediaFingerprint> fingerprint;
    bool cache_hit = false;
};

enum class ScanStage : std::uint8_t {
    Enumeration,
    Fingerprint,
    CacheRead,
    CacheWrite
};

struct ScanIssue {
    std::filesystem::path path;
    ScanStage stage;
    std::string message;
};

struct ScanResult {
    std::vector<ScannedFile> files;
    std::vector<ScanIssue> issues;
};

[[nodiscard]] GHIDRAENGINE_EXPORT ScanResult scan_media(
    std::span<const std::filesystem::path> roots,
    const ScanOptions &options = {},
    const FingerprintCache *cache = nullptr,
    const std::stop_token &stop = {}
);

[[nodiscard]] GHIDRAENGINE_EXPORT std::vector<std::vector<MediaId>>
exact_duplicate_groups(std::span<const ScannedFile> files);
}
#endif
