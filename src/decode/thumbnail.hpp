// The one intermediate representation every decoder produces; everything
// downstream sees only this.
#pragma once

#include <array>
#include <cstdint>

#include "ghidraengine/types.hpp"

namespace ghidraengine {

// Chroma at 8x8: it only feeds a 4x4 colour-moment grid and is low-frequency.
inline constexpr std::size_t kChromaSize = 8;

struct Thumbnail {
    // 32x32 row-major, already rotated to upright.
    std::array<std::uint8_t, kThumbSize * kThumbSize> gray{};

    // 8x8 row-major, centred on 128.
    std::array<std::uint8_t, kChromaSize * kChromaSize> cb{};
    std::array<std::uint8_t, kChromaSize * kChromaSize> cr{};

    // False for grayscale, so the colour check is skipped rather than comparing
    // two flat 128 planes and calling it a match.
    bool has_color = false;

    // Before any scaling.
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
};

} // namespace ghidraengine
