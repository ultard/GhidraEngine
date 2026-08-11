#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "ghidraengine/error.hpp"
#include "core/platform.hpp"

namespace ghidraengine {

inline constexpr std::size_t kHeaderBytes = 512;

Result<std::size_t> read_header(const platform::File& file,
                                std::span<std::uint8_t, kHeaderBytes> out);

Result<void> read_entire_file(const std::filesystem::path& path,
                              std::vector<std::uint8_t>& buffer,
                              std::uint64_t max_size);

}
