#pragma once

#include <cstddef>
#include <cstdint>

#include "decode/thumbnail.hpp"

namespace ghidraengine {

// Box-averages one channel of an interleaved buffer. Every source pixel
// contributes exactly once, which is what makes the result stable across two
// encodes of the same picture at different resolutions.
void box_resample_channel(const std::uint8_t* source, std::size_t source_width,
                          std::size_t source_height, std::size_t source_stride,
                          std::size_t channels, std::size_t channel_index,
                          std::uint8_t* destination, std::size_t dst_width,
                          std::size_t dst_height);

// EXIF orientation 1-8, so a copy carrying only a rotation flag hashes the same
// as a physically rotated one.
void apply_exif_orientation(Thumbnail& thumb, std::uint16_t orientation);

constexpr bool orientation_transposes(std::uint16_t orientation) noexcept {
    return orientation >= 5 && orientation <= 8;
}

} // namespace ghidraengine
