#include "kernels.hpp"

#include <immintrin.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#define GHIDRAENGINE_POPCNT __attribute__((target("popcnt")))
#define GHIDRAENGINE_AVX2 __attribute__((target("avx2")))

namespace GhidraEngine::detail {

namespace {

GHIDRAENGINE_POPCNT unsigned pop_distance(const PdqHash &a, const PdqHash &b) noexcept {
    unsigned distance = 0;

    for (std::size_t i = 0; i < 4; ++i) {
        distance += __builtin_popcountll(a.words[i] ^ b.words[i]);
    }

    return distance;
}

GHIDRAENGINE_POPCNT unsigned pop_bounded(
    const PdqHash &a,
    const PdqHash &b,
    unsigned threshold
) noexcept {
    unsigned distance = 0;

    for (std::size_t i = 0; i < 4; ++i) {
        distance += __builtin_popcountll(a.words[i] ^ b.words[i]);

        if (distance > threshold) {
            break;
        }
    }

    return distance;
}

GHIDRAENGINE_POPCNT void pop_distances(
    const PdqHash &query,
    const void *data,
    std::size_t stride,
    std::span<std::uint16_t> output,
    unsigned threshold
) noexcept {
    const auto *bytes = static_cast<const std::uint8_t *>(data);

    for (std::size_t i = 0; i < output.size(); ++i) {
        PdqHash hash;
        std::memcpy(hash.words.data(), bytes + i * stride, sizeof(hash));
        output[i] = static_cast<std::uint16_t>(pop_bounded(query, hash, threshold));
    }
}

GHIDRAENGINE_POPCNT bool pop_contains(
    const PdqHash &query,
    std::span<const PdqHash> hashes,
    unsigned threshold
) noexcept {
    for (const auto &hash : hashes) {
        if (pop_bounded(query, hash, threshold) <= threshold) {
            return true;
        }
    }

    return false;
}

GHIDRAENGINE_AVX2 unsigned avx_count(__m256i bits) noexcept {
    const auto lookup = _mm256_setr_epi8(
        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
    );
    const auto mask = _mm256_set1_epi8(15);
    const auto low = _mm256_shuffle_epi8(lookup, _mm256_and_si256(bits, mask));
    const auto high = _mm256_shuffle_epi8(
        lookup,
        _mm256_and_si256(_mm256_srli_epi16(bits, 4), mask)
    );
    const auto sums = _mm256_sad_epu8(_mm256_add_epi8(low, high), _mm256_setzero_si256());

    return static_cast<unsigned>(
        _mm256_extract_epi64(sums, 0) + _mm256_extract_epi64(sums, 1) +
        _mm256_extract_epi64(sums, 2) + _mm256_extract_epi64(sums, 3)
    );
}

GHIDRAENGINE_AVX2 unsigned avx_distance(const PdqHash &a, const PdqHash &b) noexcept {
    const auto left = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a.words.data()));
    const auto right = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b.words.data()));

    return avx_count(_mm256_xor_si256(left, right));
}

GHIDRAENGINE_AVX2 unsigned avx_bounded(
    const PdqHash &a,
    const PdqHash &b,
    unsigned
) noexcept {
    return avx_distance(a, b);
}

GHIDRAENGINE_AVX2 void avx_distances(
    const PdqHash &query,
    const void *data,
    std::size_t stride,
    std::span<std::uint16_t> output,
    unsigned
) noexcept {
    const auto left = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(query.words.data()));
    const auto *bytes = static_cast<const std::uint8_t *>(data);

    for (std::size_t i = 0; i < output.size(); ++i) {
        const auto right = _mm256_loadu_si256(
            reinterpret_cast<const __m256i *>(bytes + i * stride)
        );
        output[i] = static_cast<std::uint16_t>(avx_count(_mm256_xor_si256(left, right)));
    }
}

GHIDRAENGINE_AVX2 bool avx_contains(
    const PdqHash &query,
    std::span<const PdqHash> hashes,
    unsigned threshold
) noexcept {
    const auto left = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(query.words.data()));

    for (const auto &hash : hashes) {
        const auto right = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(hash.words.data()));

        if (avx_count(_mm256_xor_si256(left, right)) <= threshold) {
            return true;
        }
    }

    return false;
}

GHIDRAENGINE_AVX2 bool avx_valid_luma(std::span<const float> luma) noexcept {
    const auto zero = _mm256_setzero_ps();
    const auto maximum = _mm256_set1_ps(255);
    std::size_t i = 0;

    for (; i + 8 <= luma.size(); i += 8) {
        const auto value = _mm256_loadu_ps(luma.data() + i);
        const auto valid = _mm256_and_ps(
            _mm256_cmp_ps(value, zero, _CMP_GE_OQ),
            _mm256_cmp_ps(value, maximum, _CMP_LE_OQ)
        );

        if (_mm256_movemask_ps(valid) != 255) {
            return false;
        }
    }

    return scalar_valid_luma(luma.subspan(i));
}

GHIDRAENGINE_AVX2 void avx_luma8(
    const std::uint8_t *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept {
    std::size_t x = 0;

    if (bands == 4) {
        const auto mask = _mm256_set1_epi32(255);

        for (; x + 8 <= luma.size(); x += 8) {
            const auto packed = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row + x * 4));
            const auto alpha = _mm256_srli_epi32(packed, 24);
            const auto inverse = _mm256_sub_epi32(mask, alpha);
            __m256i rgb[3]{
                _mm256_and_si256(packed, mask),
                _mm256_and_si256(_mm256_srli_epi32(packed, 8), mask),
                _mm256_and_si256(_mm256_srli_epi32(packed, 16), mask)
            };

            const bool opaque = _mm256_movemask_ps(
                _mm256_castsi256_ps(_mm256_cmpeq_epi32(alpha, mask))
            ) == 255;

            for (std::size_t c = 0; !opaque && c < 3; ++c) {
                auto sum = _mm256_add_epi32(
                    _mm256_mullo_epi32(rgb[c], alpha),
                    _mm256_mullo_epi32(_mm256_set1_epi32(background[c]), inverse)
                );
                sum = _mm256_add_epi32(sum, _mm256_set1_epi32(127));
                // Exact unsigned division by 255 for the alpha-composition numerator.
                rgb[c] = _mm256_srli_epi32(
                    _mm256_add_epi32(
                        _mm256_add_epi32(sum, _mm256_srli_epi32(sum, 8)),
                        _mm256_set1_epi32(1)
                    ),
                    8
                );
            }

            const auto red = _mm256_mul_ps(_mm256_cvtepi32_ps(rgb[0]), _mm256_set1_ps(0.299F));
            const auto green = _mm256_mul_ps(_mm256_cvtepi32_ps(rgb[1]), _mm256_set1_ps(0.587F));
            const auto blue = _mm256_mul_ps(_mm256_cvtepi32_ps(rgb[2]), _mm256_set1_ps(0.114F));
            _mm256_storeu_ps(luma.data() + x, _mm256_add_ps(_mm256_add_ps(red, green), blue));
        }
    } else {
        // A 16-byte load spans six RGB pixels; process four and leave a safe scalar tail.
        for (; x + 6 <= luma.size(); x += 4) {
            const auto packed = _mm_loadu_si128(reinterpret_cast<const __m128i *>(row + x * 3));
            const auto red = _mm_cvtepu8_epi32(_mm_shuffle_epi8(
                packed,
                _mm_setr_epi8(0, 3, 6, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1)
            ));
            const auto green = _mm_cvtepu8_epi32(_mm_shuffle_epi8(
                packed,
                _mm_setr_epi8(1, 4, 7, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1)
            ));
            const auto blue = _mm_cvtepu8_epi32(_mm_shuffle_epi8(
                packed,
                _mm_setr_epi8(2, 5, 8, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1)
            ));
            const auto rg = _mm_add_ps(
                _mm_mul_ps(_mm_cvtepi32_ps(red), _mm_set1_ps(0.299F)),
                _mm_mul_ps(_mm_cvtepi32_ps(green), _mm_set1_ps(0.587F))
            );
            _mm_storeu_ps(
                luma.data() + x,
                _mm_add_ps(rg, _mm_mul_ps(_mm_cvtepi32_ps(blue), _mm_set1_ps(0.114F)))
            );
        }
    }

    scalar_luma8(row + x * bands, luma.subspan(x), bands, background);
}

GHIDRAENGINE_AVX2 void avx_luma16(
    const std::uint16_t *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept {
    const auto step = static_cast<int>(bands * 2);
    const auto offsets = _mm256_setr_epi32(
        0,
        step,
        step * 2,
        step * 3,
        step * 4,
        step * 5,
        step * 6,
        step * 7
    );
    const auto mask = _mm256_set1_epi32(65535);
    std::size_t x = 0;
    const auto minimum = bands == 4 ? 8U : 9U;

    for (; x + minimum <= luma.size(); x += 8) {
        const auto *pixel = row + x * bands;
        const auto rg = _mm256_i32gather_epi32(reinterpret_cast<const int *>(pixel), offsets, 1);
        const auto ba = _mm256_i32gather_epi32(
            reinterpret_cast<const int *>(pixel + 2),
            offsets,
            1
        );
        __m256i rgb[3]{
            _mm256_and_si256(rg, mask),
            _mm256_srli_epi32(rg, 16),
            _mm256_and_si256(ba, mask)
        };

        if (bands == 4) {
            const auto alpha = _mm256_srli_epi32(ba, 16);
            const auto inverse = _mm256_sub_epi32(mask, alpha);
            const bool opaque = _mm256_movemask_ps(
                _mm256_castsi256_ps(_mm256_cmpeq_epi32(alpha, mask))
            ) == 255;

            for (std::size_t c = 0; !opaque && c < 3; ++c) {
                auto sum = _mm256_add_epi32(
                    _mm256_mullo_epi32(rgb[c], alpha),
                    _mm256_mullo_epi32(_mm256_set1_epi32(background[c]), inverse)
                );
                sum = _mm256_add_epi32(sum, _mm256_set1_epi32(32767));
                rgb[c] = _mm256_srli_epi32(
                    _mm256_add_epi32(
                        _mm256_add_epi32(sum, _mm256_srli_epi32(sum, 16)),
                        _mm256_set1_epi32(1)
                    ),
                    16
                );
            }
        }

        const auto red = _mm256_mul_ps(_mm256_cvtepi32_ps(rgb[0]), _mm256_set1_ps(0.299F));
        const auto green = _mm256_mul_ps(_mm256_cvtepi32_ps(rgb[1]), _mm256_set1_ps(0.587F));
        const auto blue = _mm256_mul_ps(_mm256_cvtepi32_ps(rgb[2]), _mm256_set1_ps(0.114F));
        _mm256_storeu_ps(luma.data() + x, _mm256_add_ps(_mm256_add_ps(red, green), blue));
    }

    scalar_luma16(row + x * bands, luma.subspan(x), bands, background);
}

}

const CpuKernels &popcnt_kernels() noexcept {
    static const CpuKernels kernels{
        "popcnt",
        pop_distance,
        pop_bounded,
        pop_distances,
        pop_contains,
        scalar_luma8,
        scalar_luma16,
        scalar_valid_luma
    };

    return kernels;
}

const CpuKernels &avx2_kernels() noexcept {
    static const CpuKernels kernels{
        "avx2",
        avx_distance,
        avx_bounded,
        avx_distances,
        avx_contains,
        avx_luma8,
        avx_luma16,
        avx_valid_luma
    };

    return kernels;
}

}
