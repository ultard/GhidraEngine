#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>

namespace GhidraEngine::detail {

inline std::int64_t seek_video_input(
    std::ifstream &input,
    const std::int64_t size,
    const std::int64_t offset,
    const int origin
) {
    input.clear();
    std::int64_t base;

    switch (origin) {
        case SEEK_SET: base = 0; break;
        case SEEK_END: base = size; break;
        case SEEK_CUR: base = static_cast<std::int64_t>(input.tellg()); break;
        default: return -1;
    }

    if (base < 0 || base > size || offset < -base || offset > size - base)
        return -1;

    input.seekg(base + offset, std::ios::beg);

    if (input) {
        return input.tellg();
    }

    return -1;
}

} // namespace GhidraEngine::detail
