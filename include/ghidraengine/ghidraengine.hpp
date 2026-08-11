#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

#include "ghidraengine/config.hpp"
#include "ghidraengine/export.hpp"
#include "ghidraengine/types.hpp"

namespace ghidraengine {

// CMake parses these three lines for project(VERSION); keep the format.
struct Version {
    static constexpr int major = 0;
    static constexpr int minor = 1;
    static constexpr int patch = 0;
};

GHIDRAENGINE_API const char* version_string() noexcept;

GHIDRAENGINE_API const char* active_simd_backend() noexcept;

class GHIDRAENGINE_API Scanner {
public:
    explicit Scanner(ScanConfig config = {});
    ~Scanner();

    Scanner(Scanner&&) noexcept;
    Scanner& operator=(Scanner&&) noexcept;
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    [[nodiscard]] Result<Report> scan(std::span<const std::filesystem::path> roots);
    [[nodiscard]] Result<Report> scan(std::span<const std::filesystem::path> roots,
                                      std::stop_token token);

    [[nodiscard]] const ScanConfig& config() const noexcept;
    void set_config(ScanConfig config);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

GHIDRAENGINE_API Result<Hash128> hash_file(const std::filesystem::path& path);

GHIDRAENGINE_API Result<Hash128> hash_file_partial(const std::filesystem::path& path,
                                              std::uint64_t size);

GHIDRAENGINE_API Result<ImageSignature> compute_image_signature(
    const std::filesystem::path& path, const ImageMatchConfig& config = {});

GHIDRAENGINE_API Result<VideoSignature> compute_video_signature(
    const std::filesystem::path& path, const VideoMatchConfig& config = {});

GHIDRAENGINE_API ImageSignature signature_from_thumbnail(std::span<const std::uint8_t> pixels,
                                                    const ImageMatchConfig& config = {});

GHIDRAENGINE_API std::uint32_t hamming_distance(std::uint64_t a, std::uint64_t b) noexcept;
GHIDRAENGINE_API std::uint32_t hamming_distance(const Hash256& a, const Hash256& b) noexcept;

GHIDRAENGINE_API bool images_match(const ImageSignature& a, const ImageSignature& b,
                              const ImageMatchConfig& config) noexcept;

GHIDRAENGINE_API double video_similarity(const VideoSignature& a, const VideoSignature& b,
                                    const VideoMatchConfig& config) noexcept;

GHIDRAENGINE_API MediaKind probe_media_kind(const std::filesystem::path& path);
GHIDRAENGINE_API MediaKind probe_media_kind(std::span<const std::uint8_t> header) noexcept;

}
