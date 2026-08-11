#include "hash/phash.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>

#include "ghidraengine/ghidraengine.hpp"
#include "hash/dct.hpp"

namespace ghidraengine {
namespace {

constexpr std::size_t kThumbPixels = kThumbSize * kThumbSize;

using GrayBuffer = std::array<std::uint8_t, kThumbPixels>;

float ac_median(const float* coefficients, std::size_t block, std::size_t stride) {
    std::array<float, kDctOutputSize * kDctOutputSize> values{};
    std::size_t count = 0;
    for (std::size_t v = 0; v < block; ++v) {
        for (std::size_t u = 0; u < block; ++u) {
            if (v == 0 && u == 0) {
                continue;
            }
            values[count++] = coefficients[v * stride + u];
        }
    }

    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(count / 2);
    std::nth_element(values.begin(), middle, values.begin() + static_cast<std::ptrdiff_t>(count));
    return *middle;
}

std::uint64_t extract_phash64(const float* coefficients) {
    const float median = ac_median(coefficients, 8, kDctOutputSize);
    std::uint64_t hash = 0;
    for (std::size_t v = 0; v < 8; ++v) {
        for (std::size_t u = 0; u < 8; ++u) {
            if (coefficients[v * kDctOutputSize + u] > median) {
                hash |= 1ULL << (v * 8 + u);
            }
        }
    }
    return hash;
}

Hash256 extract_phash256(const float* coefficients) {
    const float median = ac_median(coefficients, kDctOutputSize, kDctOutputSize);
    Hash256 hash{};
    for (std::size_t v = 0; v < kDctOutputSize; ++v) {
        for (std::size_t u = 0; u < kDctOutputSize; ++u) {
            if (coefficients[v * kDctOutputSize + u] > median) {
                const std::size_t bit = v * kDctOutputSize + u;
                hash[bit / 64] |= 1ULL << (bit % 64);
            }
        }
    }
    return hash;
}

void box_resample(std::span<const std::uint8_t> source, std::size_t width, std::size_t height,
                  std::uint8_t* destination) {
    for (std::size_t row = 0; row < height; ++row) {
        const std::size_t y0 = row * kThumbSize / height;
        const std::size_t y1 = std::max(y0 + 1, (row + 1) * kThumbSize / height);

        for (std::size_t col = 0; col < width; ++col) {
            const std::size_t x0 = col * kThumbSize / width;
            const std::size_t x1 = std::max(x0 + 1, (col + 1) * kThumbSize / width);

            std::uint32_t sum = 0;
            for (std::size_t y = y0; y < y1; ++y) {
                for (std::size_t x = x0; x < x1; ++x) {
                    sum += source[y * kThumbSize + x];
                }
            }
            const std::uint32_t area = static_cast<std::uint32_t>((y1 - y0) * (x1 - x0));
            destination[row * width + col] = static_cast<std::uint8_t>((sum + area / 2) / area);
        }
    }
}

std::uint64_t extract_dhash64(std::span<const std::uint8_t> gray) {
    std::array<std::uint8_t, 9 * 8> small{};
    box_resample(gray, 9, 8, small.data());

    std::uint64_t hash = 0;
    std::size_t bit = 0;
    for (std::size_t row = 0; row < 8; ++row) {
        for (std::size_t col = 0; col < 8; ++col) {
            if (small[row * 9 + col] > small[row * 9 + col + 1]) {
                hash |= 1ULL << bit;
            }
            ++bit;
        }
    }
    return hash;
}

std::array<std::uint8_t, kColorMomentBytes> extract_color_moments(
    const std::array<std::uint8_t, kChromaSize * kChromaSize>& cb,
    const std::array<std::uint8_t, kChromaSize * kChromaSize>& cr) {
    std::array<std::uint8_t, kColorMomentBytes> moments{};

    const auto fill = [](const std::array<std::uint8_t, kChromaSize * kChromaSize>& plane,
                         std::uint8_t* out) {
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                const std::size_t base = (row * 2) * kChromaSize + (col * 2);
                const std::uint32_t sum = plane[base] + plane[base + 1] +
                                          plane[base + kChromaSize] +
                                          plane[base + kChromaSize + 1];
                out[row * 4 + col] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    };

    fill(cb, moments.data());
    fill(cr, moments.data() + 16);
    return moments;
}

void apply_dihedral(const std::uint8_t* source, std::size_t size, std::size_t variant,
                    std::uint8_t* destination) {
    for (std::size_t y = 0; y < size; ++y) {
        for (std::size_t x = 0; x < size; ++x) {
            std::size_t sx = x;
            std::size_t sy = y;
            switch (variant) {
                case 0: break;
                case 1: sx = size - 1 - x; break;
                case 2: sy = size - 1 - y; break;
                case 3: sx = size - 1 - x; sy = size - 1 - y; break;
                case 4: sx = y; sy = x; break;
                case 5: sx = y; sy = size - 1 - x; break;
                case 6: sx = size - 1 - y; sy = x; break;
                default: sx = size - 1 - y; sy = size - 1 - x; break;
            }
            destination[y * size + x] = source[sy * size + sx];
        }
    }
}

std::size_t canonical_variant(const float* coefficients) {
    const float gx = coefficients[1];
    const float gy = coefficients[kDctOutputSize];
    const bool transpose = std::abs(gx) < std::abs(gy);
    const float first = transpose ? gy : gx;
    const float second = transpose ? gx : gy;
    return (transpose ? 4U : 0U) + (first < 0.0F ? 1U : 0U) + (second < 0.0F ? 2U : 0U);
}

void to_float(std::span<const std::uint8_t> source, float* destination) {
    for (std::size_t i = 0; i < kThumbPixels; ++i) {
        destination[i] = static_cast<float>(source[i]);
    }
}

}

std::uint64_t phash64_of_gray(std::span<const std::uint8_t> gray) noexcept {
    if (gray.size() < kThumbPixels) {
        return 0;
    }
    alignas(64) float input[kDctInputCount];
    alignas(64) float coefficients[kDctOutputCount];
    to_float(gray, input);
    dct16()(input, coefficients);
    return extract_phash64(coefficients);
}

double luma_variance(std::span<const std::uint8_t> gray) noexcept {
    if (gray.empty()) {
        return 0.0;
    }
    double mean = 0.0;
    for (const std::uint8_t value : gray) {
        mean += value;
    }
    mean /= static_cast<double>(gray.size());

    double accumulator = 0.0;
    for (const std::uint8_t value : gray) {
        const double delta = static_cast<double>(value) - mean;
        accumulator += delta * delta;
    }
    return accumulator / static_cast<double>(gray.size());
}

std::uint32_t color_distance(const std::array<std::uint8_t, kColorMomentBytes>& a,
                             const std::array<std::uint8_t, kColorMomentBytes>& b) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < kColorMomentBytes; ++i) {
        sum += static_cast<std::uint32_t>(std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return sum / static_cast<std::uint32_t>(kColorMomentBytes);
}

ImageSignature compute_signature(const Thumbnail& thumb, const ImageMatchConfig& config) {
    ImageSignature signature;
    signature.width = thumb.source_width;
    signature.height = thumb.source_height;
    signature.has_color = thumb.has_color;

    alignas(64) float input[kDctInputCount];
    alignas(64) float coefficients[kDctOutputCount];
    const Dct16Fn transform = dct16();

    to_float(thumb.gray, input);
    transform(input, coefficients);

    std::span<const std::uint8_t> gray{thumb.gray};
    GrayBuffer rotated{};
    std::size_t variant = 0;

    if (config.dihedral_invariant) {
        variant = canonical_variant(coefficients);
        if (variant != 0) {
            apply_dihedral(thumb.gray.data(), kThumbSize, variant, rotated.data());
            gray = rotated;
            to_float(gray, input);
            transform(input, coefficients);
        }
    }

    signature.phash64 = extract_phash64(coefficients);
    signature.phash256 = extract_phash256(coefficients);
    signature.dhash64 = extract_dhash64(gray);

    if (thumb.has_color) {
        if (variant == 0) {
            signature.color = extract_color_moments(thumb.cb, thumb.cr);
        } else {
            std::array<std::uint8_t, kChromaSize * kChromaSize> cb{};
            std::array<std::uint8_t, kChromaSize * kChromaSize> cr{};
            apply_dihedral(thumb.cb.data(), kChromaSize, variant, cb.data());
            apply_dihedral(thumb.cr.data(), kChromaSize, variant, cr.data());
            signature.color = extract_color_moments(cb, cr);
        }
    }

    return signature;
}

std::uint32_t hamming_distance(std::uint64_t a, std::uint64_t b) noexcept {
    return static_cast<std::uint32_t>(std::popcount(a ^ b));
}

std::uint32_t hamming_distance(const Hash256& a, const Hash256& b) noexcept {
    return static_cast<std::uint32_t>(std::popcount(a[0] ^ b[0]) + std::popcount(a[1] ^ b[1]) +
                                      std::popcount(a[2] ^ b[2]) + std::popcount(a[3] ^ b[3]));
}

bool images_match(const ImageSignature& a, const ImageSignature& b,
                  const ImageMatchConfig& config) noexcept {
    if (hamming_distance(a.phash64, b.phash64) > config.phash_threshold) {
        return false;
    }
    if (config.phash256_threshold < 256 &&
        hamming_distance(a.phash256, b.phash256) > config.phash256_threshold) {
        return false;
    }
    if (config.dhash_threshold < 64 &&
        hamming_distance(a.dhash64, b.dhash64) > config.dhash_threshold) {
        return false;
    }
    if (config.color_threshold < 255 && a.has_color && b.has_color &&
        color_distance(a.color, b.color) > config.color_threshold) {
        return false;
    }
    return true;
}

ImageSignature signature_from_thumbnail(std::span<const std::uint8_t> pixels,
                                        const ImageMatchConfig& config) {
    Thumbnail thumb;
    const std::size_t count = std::min(pixels.size(), kThumbPixels);
    std::memcpy(thumb.gray.data(), pixels.data(), count);
    thumb.has_color = false;
    thumb.source_width = kThumbSize;
    thumb.source_height = kThumbSize;
    return compute_signature(thumb, config);
}

const char* active_simd_backend() noexcept {
    return backend_name(active_backend());
}

}
