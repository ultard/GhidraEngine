#pragma once

#include <array>
#include <cstdint>

#include "ghidraengine/types.hpp"

namespace ghidraengine {

inline constexpr std::size_t kChromaSize = 8;

struct Thumbnail {
    std::array<std::uint8_t, kThumbSize * kThumbSize> gray{};

    std::array<std::uint8_t, kChromaSize * kChromaSize> cb{};
    std::array<std::uint8_t, kChromaSize * kChromaSize> cr{};

    bool has_color = false;

    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
};

}
