#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <vector>

#include "ghidraengine/config.hpp"
#include "ghidraengine/types.hpp"

namespace ghidraengine {

struct EnumerateResult {
    std::vector<FileEntry> files;
    std::vector<FileError> errors;
    std::uint64_t files_seen = 0;
    std::uint64_t hardlinks_collapsed = 0;
    bool cancelled = false;
};

// Walks `roots` depth-first. Files reachable through several paths (hard links)
// are reported once: they are one extent on disk.
EnumerateResult enumerate_files(std::span<const std::filesystem::path> roots,
                                const ScanConfig& config,
                                const std::stop_token& token);

MediaKind classify_header(std::span<const std::uint8_t> header) noexcept;

// Pre-filter only: decides which files are worth opening, never the media kind.
bool extension_is_candidate(const std::filesystem::path& path) noexcept;

} // namespace ghidraengine
