#pragma once

#include <cstdint>
#include <span>

#include "ghidraengine/config.hpp"
#include "ghidraengine/types.hpp"
#include "decode/thumbnail.hpp"

namespace ghidraengine {

ImageSignature compute_signature(const Thumbnail& thumb, const ImageMatchConfig& config);

std::uint64_t phash64_of_gray(std::span<const std::uint8_t> gray) noexcept;

double luma_variance(std::span<const std::uint8_t> gray) noexcept;

std::uint32_t color_distance(const std::array<std::uint8_t, kColorMomentBytes>& a,
                             const std::array<std::uint8_t, kColorMomentBytes>& b) noexcept;

}
