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

EnumerateResult enumerate_files(std::span<const std::filesystem::path> roots,
                                const ScanConfig& config,
                                const std::stop_token& token);

MediaKind classify_header(std::span<const std::uint8_t> header) noexcept;

bool extension_is_candidate(const std::filesystem::path& path) noexcept;

}
