#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "ghidraengine/error.hpp"
#include "ghidraengine/types.hpp"
#include "core/platform.hpp"

namespace ghidraengine {

inline constexpr std::size_t kPartialHashChunk = 64 * 1024;

inline constexpr std::size_t kStreamBufferSize = 1024 * 1024;

Hash128 hash_bytes(std::span<const std::uint8_t> data) noexcept;

Result<Hash128> hash_partial(const platform::File& file, std::uint64_t size,
                             std::span<std::uint8_t> scratch);

Result<Hash128> hash_full(const platform::File& file, std::span<std::uint8_t> scratch,
                          std::uint64_t* bytes_read);

constexpr bool partial_hash_is_complete(std::uint64_t size) noexcept {
    return size <= 2 * kPartialHashChunk;
}

Result<bool> files_identical(const std::filesystem::path& a, const std::filesystem::path& b,
                             std::uint64_t size);

}
