#include "../../src/cpu/kernels.hpp"
#include "../support/test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::detail;
using namespace GhidraEngine::test_support;

constexpr std::array backends{
    CpuBackend::Scalar,
    CpuBackend::Popcnt,
    CpuBackend::Avx2,
    CpuBackend::Neon
};
static_assert(pdq_distance({}, {}).value() == 0);
constexpr PdqHash all_bits{{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
static_assert(pdq_distance({}, all_bits).value() == 256);
static_assert(noexcept(pdq_distance({}, {})));

TEST(CpuKernels, DistancesAndThresholdsMatchIndependentOracle) {
    std::mt19937_64 random(0x73696d64);

    for (auto backend : backends) {
        const auto *kernels = cpu_kernels_for(backend);

        if (!kernels) {
            continue;
        }

        SCOPED_TRACE(kernels->name);

        for (unsigned bits = 0; bits <= 256; ++bits) {
            const auto a = random_hash(random);
            auto b = a;
            const auto flips = prefix_hash(bits);

            for (std::size_t i = 0; i < 4; ++i) {
                b.words[i] ^= flips.words[i];
            }

            EXPECT_EQ(kernels->distance(a, b), bit_distance(a, b));

            for (unsigned threshold = 0; threshold <= 256; ++threshold) {
                const auto distance = kernels->bounded(a, b, threshold);

                EXPECT_EQ(distance <= threshold, bits <= threshold);

                if (bits <= threshold) {
                    EXPECT_EQ(distance, bits);
                }
            }
        }

        for (std::size_t count : {0U, 1U, 7U, 8U, 17U, 63U, 64U, 65U}) {
            for (std::size_t stride : {32U, 40U, 48U}) {
                std::vector<std::uint8_t> bytes(1 + count * stride);
                std::vector<PdqHash> hashes(count);
                std::vector<std::uint16_t> output(count);
                const auto query = random_hash(random);

                for (std::size_t i = 0; i < count; ++i) {
                    hashes[i] = i == count / 2 ? query : random_hash(random);
                    std::memcpy(bytes.data() + 1 + i * stride, &hashes[i], sizeof(PdqHash));
                }

                for (unsigned threshold : {0U, 31U, 128U, 256U}) {
                    kernels->distances(query, bytes.data() + 1, stride, output, threshold);
                    bool expected_match = false;

                    for (std::size_t i = 0; i < count; ++i) {
                        const auto expected = bit_distance(query, hashes[i]);
                        expected_match |= expected <= threshold;
                        EXPECT_EQ(output[i] <= threshold, expected <= threshold);

                        if (expected <= threshold) {
                            EXPECT_EQ(output[i], expected);
                        }
                    }

                    EXPECT_EQ(kernels->contains(query, hashes, threshold), expected_match);
                }
            }
        }
    }
}

template <class Channel>
void check_pixels() {
    const auto &scalar = *cpu_kernels_for(CpuBackend::Scalar);
    std::mt19937_64 random(0x6c756d61);

    for (auto backend : backends) {
        const auto *kernels = cpu_kernels_for(backend);

        if (!kernels) {
            continue;
        }

        SCOPED_TRACE(kernels->name);

        for (unsigned bands : {3U, 4U}) {
            for (std::size_t count = 0; count < 82; ++count) {
                std::vector<Channel> pixels(1 + count * bands);

                for (auto &sample : pixels) {
                    sample = static_cast<Channel>(random());
                }

                if (bands == 4 && count % 3 != 2) {
                    for (std::size_t i = 0; i < count; ++i) {
                        pixels[1 + i * bands + 3] = count % 3 == 0 ?
                            std::numeric_limits<Channel>::max() : 0;
                    }
                }

                constexpr std::array backgrounds{
                    std::array<std::uint8_t, 3>{0, 0, 0},
                    std::array<std::uint8_t, 3>{255, 255, 255},
                    std::array<std::uint8_t, 3>{17, 128, 231}
                };

                for (const auto background : backgrounds) {
                    std::vector<float> expected(count), actual(count);

                    if constexpr (sizeof(Channel) == 1) {
                        scalar.luma8(pixels.data() + 1, expected, bands, background);
                        kernels->luma8(pixels.data() + 1, actual, bands, background);
                    } else {
                        scalar.luma16(pixels.data() + 1, expected, bands, background);
                        kernels->luma16(pixels.data() + 1, actual, bands, background);
                    }

                    EXPECT_EQ(actual, expected);
                }
            }
        }
    }
}

TEST(CpuKernels, PixelBlocksAndTailsExactlyMatchScalar) {
    check_pixels<std::uint8_t>();
    check_pixels<std::uint16_t>();

    // All alpha values exercise the exact integer division, including near-opaque rounding.
    std::vector<std::uint16_t> pixels(65536 * 4);

    for (unsigned alpha = 0; alpha < 65536; ++alpha) {
        pixels[alpha * 4] = static_cast<std::uint16_t>(alpha ^ 0xffffU);
        pixels[alpha * 4 + 1] = 65535;
        pixels[alpha * 4 + 2] = static_cast<std::uint16_t>(alpha);
        pixels[alpha * 4 + 3] = static_cast<std::uint16_t>(alpha);
    }

    const std::array<std::uint8_t, 3> background{17, 128, 231};
    std::vector<float> expected(65536), actual(65536);
    cpu_kernels_for(CpuBackend::Scalar)->luma16(pixels.data(), expected, 4, background);

    for (auto backend : backends) {
        if (const auto *kernels = cpu_kernels_for(backend)) {
            kernels->luma16(pixels.data(), actual, 4, background);
            EXPECT_EQ(actual, expected) << kernels->name;
        }
    }
}

TEST(CpuKernels, ValidationRejectsInvalidValuesInEveryLaneAndTail) {
    for (auto backend : backends) {
        const auto *kernels = cpu_kernels_for(backend);

        if (!kernels) {
            continue;
        }

        for (std::size_t count = 0; count < 34; ++count) {
            std::vector<float> values(count + 1, 128);
            auto input = std::span(values).subspan(1);

            EXPECT_TRUE(kernels->valid_luma(input));

            for (std::size_t i = 0; i < count; ++i) {
                constexpr std::array invalid_values{
                    -0.001F,
                    255.001F,
                    std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::quiet_NaN()
                };

                for (float value : invalid_values) {
                    input[i] = value;
                    EXPECT_FALSE(kernels->valid_luma(input));
                }

                input[i] = i % 2 == 0 ? -0.0F : 255.0F;
                EXPECT_TRUE(kernels->valid_luma(input));
            }
        }
    }
}

}
