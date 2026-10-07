#ifndef GHIDRAENGINE_CPU_KERNELS_HPP
#define GHIDRAENGINE_CPU_KERNELS_HPP

#include <GhidraEngine/hash/pdq.hpp>
#include <GhidraEngine/index/flat_pdq.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace GhidraEngine::detail {

enum class CpuBackend { Scalar, Popcnt, Avx2, Neon };

struct CpuKernels {
    const char *name;
    unsigned (*distance)(const PdqHash &, const PdqHash &) noexcept;
    unsigned (*bounded)(const PdqHash &, const PdqHash &, unsigned) noexcept;

    void (*distances)(
        const PdqHash &,
        const void *,
        std::size_t,
        std::span<std::uint16_t>,
        unsigned
    ) noexcept;

    bool (*contains)(
        const PdqHash &,
        std::span<const PdqHash>,
        unsigned
    ) noexcept;

    void (*luma8)(
        const std::uint8_t *,
        std::span<float>,
        unsigned,
        const std::array<std::uint8_t, 3> &
    ) noexcept;

    void (*luma16)(
        const std::uint16_t *,
        std::span<float>,
        unsigned,
        const std::array<std::uint8_t, 3> &
    ) noexcept;

    bool (*valid_luma)(std::span<const float>) noexcept;
};

GHIDRAENGINE_EXPORT const CpuKernels &cpu_kernels() noexcept;
GHIDRAENGINE_EXPORT const CpuKernels *cpu_kernels_for(CpuBackend backend) noexcept;
GHIDRAENGINE_EXPORT std::vector<PdqHit> search_pdq_entries(
    std::span<const PdqIndexEntry> entries,
    const PdqHash &query,
    unsigned threshold
);

unsigned scalar_distance(const PdqHash &a, const PdqHash &b) noexcept;
unsigned scalar_bounded(const PdqHash &a, const PdqHash &b, unsigned threshold) noexcept;

void scalar_luma8(
    const std::uint8_t *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept;

void scalar_luma16(
    const std::uint16_t *row,
    std::span<float> luma,
    unsigned bands,
    const std::array<std::uint8_t, 3> &background
) noexcept;
bool scalar_valid_luma(std::span<const float> luma) noexcept;

const CpuKernels &popcnt_kernels() noexcept;
const CpuKernels &avx2_kernels() noexcept;
const CpuKernels &neon_kernels() noexcept;

}

#endif
