#pragma once

#include <cstdint>
#include <span>

#include "ghidraengine/config.hpp"
#include "ghidraengine/types.hpp"
#include "decode/thumbnail.hpp"

namespace ghidraengine {

// All four hashes from one thumbnail. Decoding dominates by two orders of
// magnitude, so the extra three are close to free.
ImageSignature compute_signature(const Thumbnail& thumb, const ImageMatchConfig& config);

// For video keyframes, where only the DCT hash is needed.
std::uint64_t phash64_of_gray(std::span<const std::uint8_t> gray) noexcept;

// Low variance means a flat frame (black, fade), which hashes indiscriminately.
double luma_variance(std::span<const std::uint8_t> gray) noexcept;

// Mean absolute difference between two colour-moment vectors, 0-255.
std::uint32_t color_distance(const std::array<std::uint8_t, kColorMomentBytes>& a,
                             const std::array<std::uint8_t, kColorMomentBytes>& b) noexcept;

} // namespace ghidraengine
