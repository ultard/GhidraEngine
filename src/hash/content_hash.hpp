// XXH3-128, not a cryptographic hash: this detects accidental collisions, not an
// adversary. ScanConfig::verify_bytes exists for callers wanting proof rather
// than probability.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "ghidraengine/error.hpp"
#include "ghidraengine/types.hpp"
#include "core/platform.hpp"

namespace ghidraengine {

// From each end for the cheap probe: enough to catch differing container headers
// and trailing metadata, where near-identical media files actually differ.
inline constexpr std::size_t kPartialHashChunk = 64 * 1024;

// Large enough that syscall overhead disappears, small enough to stay in L2.
inline constexpr std::size_t kStreamBufferSize = 1024 * 1024;

Hash128 hash_bytes(std::span<const std::uint8_t> data) noexcept;

// At or below 2 * kPartialHashChunk this reads the whole file and is exact.
Result<Hash128> hash_partial(const platform::File& file, std::uint64_t size,
                             std::span<std::uint8_t> scratch);

// `scratch` should be kStreamBufferSize, one per thread.
Result<Hash128> hash_full(const platform::File& file, std::span<std::uint8_t> scratch,
                          std::uint64_t* bytes_read);

constexpr bool partial_hash_is_complete(std::uint64_t size) noexcept {
    return size <= 2 * kPartialHashChunk;
}

// For verify_bytes: lockstep blocks, stopping at the first difference.
Result<bool> files_identical(const std::filesystem::path& a, const std::filesystem::path& b,
                             std::uint64_t size);

} // namespace ghidraengine
