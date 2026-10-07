#ifndef GHIDRAENGINE_SCAN_SCANNER_HPP
#define GHIDRAENGINE_SCAN_SCANNER_HPP

#include <GhidraEngine/cache/fingerprint.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
    bool generate_fingerprints = true;
};

enum class ScanProgressStage : std::uint8_t {
    Enumeration,
    Processing
};

struct ScanProgress {
    ScanProgressStage stage;
    std::size_t discovered{};
    std::size_t completed{};
    std::optional<std::size_t> total;
    std::filesystem::path path;
};

using ScanProgressCallback = std::function<void(const ScanProgress &)>;

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

[[nodiscard]] GHIDRAENGINE_EXPORT ScanResult scan_media(
    std::span<const std::filesystem::path> roots,
    const ScanOptions &options,
    const FingerprintCache *cache,
    const std::stop_token &stop,
    const ScanProgressCallback &progress
);

[[nodiscard]] GHIDRAENGINE_EXPORT std::vector<std::vector<MediaId>>
exact_duplicate_groups(std::span<const ScannedFile> files);
}
#endif
