#include "kernels.hpp"

#include <arm_neon.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace GhidraEngine::detail {

namespace {

unsigned neon_distance(const PdqHash &a, const PdqHash &b) noexcept {
    const auto *left = reinterpret_cast<const std::uint8_t *>(a.words.data());
    const auto *right = reinterpret_cast<const std::uint8_t *>(b.words.data());

    return vaddlvq_u8(vcntq_u8(veorq_u8(vld1q_u8(left), vld1q_u8(right)))) +
           vaddlvq_u8(vcntq_u8(veorq_u8(vld1q_u8(left + 16), vld1q_u8(right + 16))));
}

unsigned count_bounded(
    const std::uint8_t *left,
    const std::uint8_t *right,
    unsigned threshold
) noexcept {
    const unsigned first = vaddlvq_u8(vcntq_u8(veorq_u8(vld1q_u8(left), vld1q_u8(right))));

    if (first > threshold) {
        return first;
    }

    return first + vaddlvq_u8(vcntq_u8(veorq_u8(vld1q_u8(left + 16), vld1q_u8(right + 16))));
}

unsigned neon_bounded(const PdqHash &a, const PdqHash &b, unsigned threshold) noexcept {
    return count_bounded(
        reinterpret_cast<const std::uint8_t *>(a.words.data()),
        reinterpret_cast<const std::uint8_t *>(b.words.data()),
        threshold
    );
}

void neon_distances(
    const PdqHash &query,
    const void *data,
    std::size_t stride,
    std::span<std::uint16_t> output,
    unsigned threshold
) noexcept {
    const auto *left = reinterpret_cast<const std::uint8_t *>(query.words.data());
    const auto *bytes = static_cast<const std::uint8_t *>(data);

    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = static_cast<std::uint16_t>(count_bounded(left, bytes + i * stride, threshold));
    }
}

bool neon_contains(
    const PdqHash &query,
    std::span<const PdqHash> hashes,
    unsigned threshold
) noexcept {
    for (const auto &hash : hashes) {
        if (neon_bounded(query, hash, threshold) <= threshold) {
            return true;
        }
    }

    return false;
}

template <unsigned Bits>
uint32x4_t blend(
    uint32x4_t channel,
    uint32x4_t alpha,
    std::uint8_t background
) noexcept {
    constexpr unsigned maximum = (1U << Bits) - 1;
    const auto inverse = vsubq_u32(vdupq_n_u32(maximum), alpha);
    auto sum = vaddq_u32(vmulq_u32(channel, alpha), vmulq_u32(vdupq_n_u32(background), inverse));
    sum = vaddq_u32(sum, vdupq_n_u32(maximum / 2));

    return vshrq_n_u32(vaddq_u32(vaddq_u32(sum, vshrq_n_u32(sum, Bits)), vdupq_n_u32(1)), Bits);
}

template <unsigned Bits>
void store_luma(
    float *output,
    uint32x4_t red,
    uint32x4_t green,
    uint32x4_t blue,
    uint32x4_t alpha,
    bool has_alpha,
    const std::array<std::uint8_t, 3> &background
) noexcept {
    constexpr unsigned maximum = (1U << Bits) - 1;

    if (has_alpha && vminvq_u32(alpha) != maximum) {
        red = blend<Bits>(red, alpha, background[0]);
        green = blend<Bits>(green, alpha, background[1]);
        blue = blend<Bits>(blue, alpha, background[2]);
    }

    const auto r = vmulq_n_f32(vcvtq_f32_u32(red), 0.299F);
    const auto g = vmulq_n_f32(vcvtq_f32_u32(green), 0.587F);
    const auto b = vmulq_n_f32(vcvtq_f32_u32(blue), 0.114F);
    vst1q_f32(output, vaddq_f32(vaddq_f32(r, g), b));
}

void neon_luma8(
    const std::uint8_t *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept {
    std::size_t x = 0;

    for (; x + 8 <= luma.size(); x += 8) {
        uint16x8_t channels[4];

        if (bands == 4) {
            const auto pixel = vld4_u8(row + x * bands);

            for (std::size_t c = 0; c < 4; ++c) {
                channels[c] = vmovl_u8(pixel.val[c]);
            }
        } else {
            const auto pixel = vld3_u8(row + x * bands);

            for (std::size_t c = 0; c < 3; ++c) {
                channels[c] = vmovl_u8(pixel.val[c]);
            }

            channels[3] = vdupq_n_u16(255);
        }

        store_luma<8>(
            luma.data() + x,
            vmovl_u16(vget_low_u16(channels[0])),
            vmovl_u16(vget_low_u16(channels[1])),
            vmovl_u16(vget_low_u16(channels[2])),
            vmovl_u16(vget_low_u16(channels[3])),
            bands == 4,
            background
        );
        store_luma<8>(
            luma.data() + x + 4,
            vmovl_u16(vget_high_u16(channels[0])),
            vmovl_u16(vget_high_u16(channels[1])),
            vmovl_u16(vget_high_u16(channels[2])),
            vmovl_u16(vget_high_u16(channels[3])),
            bands == 4,
            background
        );
    }

    scalar_luma8(row + x * bands, luma.subspan(x), bands, background);
}

void neon_luma16(
    const std::uint16_t *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept {
    std::size_t x = 0;

    for (; x + 4 <= luma.size(); x += 4) {
        uint16x4_t channels[4];

        if (bands == 4) {
            const auto pixel = vld4_u16(row + x * bands);

            for (std::size_t c = 0; c < 4; ++c) {
                channels[c] = pixel.val[c];
            }
        } else {
            const auto pixel = vld3_u16(row + x * bands);

            for (std::size_t c = 0; c < 3; ++c) {
                channels[c] = pixel.val[c];
            }

            channels[3] = vdup_n_u16(65535);
        }

        store_luma<16>(
            luma.data() + x,
            vmovl_u16(channels[0]),
            vmovl_u16(channels[1]),
            vmovl_u16(channels[2]),
            vmovl_u16(channels[3]),
            bands == 4,
            background
        );
    }

    scalar_luma16(row + x * bands, luma.subspan(x), bands, background);
}

bool neon_valid_luma(std::span<const float> luma) noexcept {
    std::size_t i = 0;

    for (; i + 4 <= luma.size(); i += 4) {
        const auto value = vld1q_f32(luma.data() + i);
        const auto valid = vandq_u32(
            vcgeq_f32(value, vdupq_n_f32(0)),
            vcleq_f32(value, vdupq_n_f32(255))
        );

        if (vminvq_u32(valid) != UINT32_MAX) {
            return false;
        }
    }

    return scalar_valid_luma(luma.subspan(i));
}

}

const CpuKernels &neon_kernels() noexcept {
    static const CpuKernels kernels{
        "neon",
        neon_distance,
        neon_bounded,
        neon_distances,
        neon_contains,
        neon_luma8,
        neon_luma16,
        neon_valid_luma
    };

    return kernels;
}

}
