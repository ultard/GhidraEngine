#include "kernels.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace GhidraEngine::detail {

unsigned scalar_distance(const PdqHash &a, const PdqHash &b) noexcept {
    unsigned distance = 0;

    for (std::size_t i = 0; i < 4; ++i) {
        distance += std::popcount(a.words[i] ^ b.words[i]);
    }

    return distance;
}

unsigned scalar_bounded(const PdqHash &a, const PdqHash &b, unsigned threshold) noexcept {
    unsigned distance = 0;

    for (std::size_t i = 0; i < 4; ++i) {
        distance += std::popcount(a.words[i] ^ b.words[i]);

        if (distance > threshold) {
            break;
        }
    }

    return distance;
}

namespace {

void scalar_distances(
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
        output[i] = static_cast<std::uint16_t>(scalar_bounded(query, hash, threshold));
    }
}

bool scalar_contains(
    const PdqHash &query,
    std::span<const PdqHash> hashes,
    unsigned threshold
) noexcept {
    for (const auto &hash : hashes) {
        if (scalar_bounded(query, hash, threshold) <= threshold) {
            return true;
        }
    }

    return false;
}

template <class Channel>
void scalar_luma(
    const Channel *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept {
    constexpr unsigned alpha_max = std::numeric_limits<Channel>::max();

    for (std::size_t x = 0; x < luma.size(); ++x) {
        const auto *pixel = row + x * bands;
        std::array<unsigned, 3> rgb{pixel[0], pixel[1], pixel[2]};

        if (bands == 4) {
            const unsigned alpha = pixel[3];

            for (std::size_t c = 0; c < 3; ++c) {
                rgb[c] = (rgb[c] * alpha + background[c] * (alpha_max - alpha) + alpha_max / 2) /
                         alpha_max;
            }
        }

        luma[x] = 0.299F * static_cast<float>(rgb[0]) + 0.587F * static_cast<float>(rgb[1]) +
                  0.114F * static_cast<float>(rgb[2]);
    }
}

constexpr CpuKernels scalar{
    "scalar",
    scalar_distance,
    scalar_bounded,
    scalar_distances,
    scalar_contains,
    scalar_luma8,
    scalar_luma16,
    scalar_valid_luma
};

}

void scalar_luma8(
    const std::uint8_t *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept {
    scalar_luma(row, luma, bands, background);
}

void scalar_luma16(
    const std::uint16_t *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept {
    scalar_luma(row, luma, bands, background);
}

bool scalar_valid_luma(std::span<const float> luma) noexcept {
    for (const auto value : luma) {
        if (!(value >= 0 && value <= 255)) {
            return false;
        }
    }

    return true;
}

const CpuKernels *cpu_kernels_for(CpuBackend backend) noexcept {
    if (backend == CpuBackend::Scalar) {
        return &scalar;
    }

#if defined(GHIDRAENGINE_SIMD_X86)
    static const auto features = [] {
        __builtin_cpu_init();

        return std::array{
            __builtin_cpu_supports("popcnt") != 0,
            __builtin_cpu_supports("avx2") != 0
        };
    }();

    if (backend == CpuBackend::Popcnt && features[0]) {
        return &popcnt_kernels();
    }

    if (backend == CpuBackend::Avx2 && features[1]) {
        return &avx2_kernels();
    }
#elif defined(GHIDRAENGINE_SIMD_NEON)
    if (backend == CpuBackend::Neon) {
        return &neon_kernels();
    }
#endif

    return nullptr;
}

const CpuKernels &cpu_kernels() noexcept {
    static const CpuKernels selected = [] {
        auto result = scalar;

        if (const auto *neon = cpu_kernels_for(CpuBackend::Neon)) {
            result = *neon;
        }

        if (const auto *avx = cpu_kernels_for(CpuBackend::Avx2)) {
            result.name = avx->name;
            result.luma8 = avx->luma8;
            result.luma16 = avx->luma16;
            result.valid_luma = avx->valid_luma;
        }

        if (const auto *popcnt = cpu_kernels_for(CpuBackend::Popcnt)) {
            // Pair comparisons use POPCNT; pixel/validation kernels retain AVX2.
            result.distance = popcnt->distance;
            result.bounded = popcnt->bounded;
            result.distances = popcnt->distances;
            result.contains = popcnt->contains;
            result.name = cpu_kernels_for(CpuBackend::Avx2) ? "popcnt+avx2" : "popcnt";
        }

        return result;
    }();

    return selected;
}

PdqDistance pdq_distance_runtime(const PdqHash &a, const PdqHash &b) noexcept {
    return PdqDistance{cpu_kernels().distance(a, b)};
}

std::vector<PdqHit> search_pdq_entries(
    std::span<const PdqIndexEntry> entries,
    const PdqHash &query,
    unsigned threshold
) {
    const auto distances = cpu_kernels().distances;
    std::vector<PdqHit> hits;

    if (threshold == 256) {
        hits.reserve(entries.size());
    }

    std::array<std::uint16_t, 64> block{};

    while (!entries.empty()) {
        const auto count = std::min(block.size(), entries.size());
        const auto output = std::span(block).first(count);
        distances(
            query,
            entries.front().hash.words.data(),
            sizeof(PdqIndexEntry),
            output,
            threshold
        );

        for (std::size_t i = 0; i < count; ++i) {
            if (output[i] <= threshold) {
                hits.push_back({entries[i].id, PdqDistance{output[i]}});
            }
        }

        entries = entries.subspan(count);
    }

    return hits;
}

}
