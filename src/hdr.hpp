#ifndef GHIDRAENGINE_HDR_HPP
#define GHIDRAENGINE_HDR_HPP

#include <GhidraEngine/image/decode.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace GhidraEngine::detail {

// ITU-R BT.2100 PQ EOTF, in nits. HLG inverse OETF is scene-linear.
inline double pq_nits(double encoded) {
    const auto p = std::pow(encoded, 1.0 / (2523.0 / 32.0));
    const auto numerator = std::max(p - 3424.0 / 4096.0, 0.0);
    const auto denominator = 2413.0 / 128.0 - (2392.0 / 128.0) * p;

    return 10000.0 * std::pow(numerator / denominator, 1.0 / (2610.0 / 16384.0));
}

inline double hlg_linear(double encoded) {
    constexpr double a = 0.17883277;
    constexpr double b = 1.0 - 4.0 * a;
    const auto c = 0.5 - a * std::log(4.0 * a);

    return encoded <= 0.5 ? encoded * encoded / 3.0 : (std::exp((encoded - c) / a) + b) / 12.0;
}

inline const std::array<double, 65536> &hdr_transfer_table(bool hlg) {
    const auto build = [](bool scene_linear) {
        std::array<double, 65536> table{};

        for (std::size_t i = 0; i < table.size(); ++i) {
            const auto encoded = static_cast<double>(i) / 65535.0;
            table[i] = scene_linear ? hlg_linear(encoded) : pq_nits(encoded) / 203.0;
        }

        return table;
    };

    if (hlg) {
        static const auto table = build(true);

        return table;
    }

    static const auto table = build(false);

    return table;
}

inline std::array<double, 3> bt2020_to_srgb(const std::array<double, 3> &rgb) {
    return {
        1.660491 * rgb[0] - 0.587641 * rgb[1] - 0.072850 * rgb[2],
        -0.124550 * rgb[0] + 1.132900 * rgb[1] - 0.008349 * rgb[2],
        -0.018151 * rgb[0] - 0.100579 * rgb[1] + 1.118730 * rgb[2]
    };
}

// Linear sRGB, 1.0 = 203 nits. A fixed curve keeps fingerprints independent of frame history.
// ponytail: luminance Reinhard with gamut clipping; use BT.2390 for colorimetric rendering.
inline std::array<unsigned, 3> tone_map(
    std::array<double, 3> rgb,
    const HdrToneMapOptions &options,
    double white_squared
) {
    for (auto &c : rgb) {
        if (!std::isfinite(c)) {
            throw std::invalid_argument("Non-finite HDR sample");
        }

        c = std::max(c, 0.0) * options.exposure;
    }

    const auto luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
    const auto scale = (1.0 + luminance / white_squared) / (1.0 + luminance);
    std::array<unsigned, 3> result{};

    for (std::size_t i = 0; i < 3; ++i) {
        const auto linear = std::clamp(rgb[i] * scale, 0.0, 1.0);
        const auto encoded =
            linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        result[i] = static_cast<unsigned>(std::lround(encoded * 255.0));
    }

    return result;
}

inline std::array<unsigned, 3> tone_map(
    std::array<double, 3> rgb,
    const HdrToneMapOptions &options
) {
    const auto white = options.peak_nits / 203.0;

    return tone_map(rgb, options, white * white);
}

}

#endif
