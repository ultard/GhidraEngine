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

// AC only: the DC term encodes average brightness, exactly what a perceptual hash
// must ignore, and leaving it in would move the median whenever exposure changed.
float ac_median(const float* coefficients, std::size_t block, std::size_t stride) {
    std::array<float, kDctOutputSize * kDctOutputSize> values{};
    std::size_t count = 0;
    for (std::size_t v = 0; v < block; ++v) {
        for (std::size_t u = 0; u < block; ++u) {
            if (v == 0 && u == 0) {
                continue; // DC
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
    return hash; // the DC bit is constant, so 63 bits carry the signal
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

// Area-average, not point sampling: point sampling makes the hash sensitive to
// which pixels land on the grid, the aliasing that separates two encodes.
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
    // 9 columns x 8 rows yields 8 horizontal comparisons per row = 64 bits.
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

    // 8x8 chroma down to a 4x4 grid: each cell is the mean of a 2x2 block.
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

// The 8 symmetries of a square, applied to the 32x32 buffer already in L1.
void apply_dihedral(std::span<const std::uint8_t> source, std::size_t variant,
                    std::uint8_t* destination) {
    for (std::size_t y = 0; y < kThumbSize; ++y) {
        for (std::size_t x = 0; x < kThumbSize; ++x) {
            std::size_t sx = x;
            std::size_t sy = y;
            switch (variant) {
                case 0: break;                                            // identity
                case 1: sx = kThumbSize - 1 - x; break;                   // mirror X
                case 2: sy = kThumbSize - 1 - y; break;                   // mirror Y
                case 3: sx = kThumbSize - 1 - x; sy = kThumbSize - 1 - y; break; // 180
                case 4: sx = y; sy = x; break;                            // transpose
                case 5: sx = y; sy = kThumbSize - 1 - x; break;           // rotate 90
                case 6: sx = kThumbSize - 1 - y; sy = x; break;           // rotate 270
                default: sx = kThumbSize - 1 - y; sy = kThumbSize - 1 - x; break; // anti-transpose
            }
            destination[y * kThumbSize + x] = source[sy * kThumbSize + sx];
        }
    }
}

void to_float(std::span<const std::uint8_t> source, float* destination) {
    for (std::size_t i = 0; i < kThumbPixels; ++i) {
        destination[i] = static_cast<float>(source[i]);
    }
}

} // namespace

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
    // Two-pass: the sum-of-squares shortcut loses precision on near-flat frames,
    // which is exactly the case this function exists to detect.
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

    if (!config.dihedral_invariant) {
        to_float(thumb.gray, input);
        transform(input, coefficients);
        signature.phash64 = extract_phash64(coefficients);
        signature.phash256 = extract_phash256(coefficients);
        signature.dhash64 = extract_dhash64(thumb.gray);
        if (thumb.has_color) {
            signature.color = extract_color_moments(thumb.cb, thumb.cr);
        }
        return signature;
    }

    // Canonical orientation = smallest 64-bit hash. Arbitrary but stable, so two
    // files differing only by rotation land on the same variant.
    GrayBuffer best_gray{};
    alignas(64) float best_coefficients[kDctOutputCount];
    std::uint64_t best_hash = 0;
    std::size_t best_variant = 0;
    GrayBuffer rotated{};

    for (std::size_t variant = 0; variant < 8; ++variant) {
        apply_dihedral(thumb.gray, variant, rotated.data());
        to_float(rotated, input);
        transform(input, coefficients);
        const std::uint64_t hash = extract_phash64(coefficients);

        if (variant == 0 || hash < best_hash) {
            best_hash = hash;
            best_variant = variant;
            best_gray = rotated;
            std::memcpy(best_coefficients, coefficients, sizeof(coefficients));
        }
    }

    signature.phash64 = best_hash;
    signature.phash256 = extract_phash256(best_coefficients);
    signature.dhash64 = extract_dhash64(best_gray);

    if (thumb.has_color) {
        // Chroma follows the same symmetry, or a rotated copy would match on luma
        // and then be rejected by the colour check.
        std::array<std::uint8_t, kChromaSize * kChromaSize> cb{};
        std::array<std::uint8_t, kChromaSize * kChromaSize> cr{};
        for (std::size_t y = 0; y < kChromaSize; ++y) {
            for (std::size_t x = 0; x < kChromaSize; ++x) {
                std::size_t sx = x;
                std::size_t sy = y;
                switch (best_variant) {
                    case 0: break;
                    case 1: sx = kChromaSize - 1 - x; break;
                    case 2: sy = kChromaSize - 1 - y; break;
                    case 3: sx = kChromaSize - 1 - x; sy = kChromaSize - 1 - y; break;
                    case 4: sx = y; sy = x; break;
                    case 5: sx = y; sy = kChromaSize - 1 - x; break;
                    case 6: sx = kChromaSize - 1 - y; sy = x; break;
                    default: sx = kChromaSize - 1 - y; sy = kChromaSize - 1 - x; break;
                }
                cb[y * kChromaSize + x] = thumb.cb[sy * kChromaSize + sx];
                cr[y * kChromaSize + x] = thumb.cr[sy * kChromaSize + sx];
            }
        }
        signature.color = extract_color_moments(cb, cr);
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
    // Cheapest first: one popcount rejects the overwhelming majority of pairs.
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
    // A grayscale scan of a colour photo is still the same picture.
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

} // namespace ghidraengine
