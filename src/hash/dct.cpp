#include "hash/dct.hpp"

#include <cmath>

#if defined(GHIDRAENGINE_X86_SIMD)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <immintrin.h>
#endif
#endif

namespace ghidraengine {
namespace {

DctTables build_tables() noexcept {
    DctTables tables{};
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t u = 0; u < kDctOutputSize; ++u) {
        for (std::size_t y = 0; y < kDctInputSize; ++y) {
            const double value =
                std::cos((2.0 * static_cast<double>(y) + 1.0) * static_cast<double>(u) * pi /
                         (2.0 * static_cast<double>(kDctInputSize)));
            tables.row_basis[u * kDctInputSize + y] = static_cast<float>(value);
            tables.col_basis[y * kDctOutputSize + u] = static_cast<float>(value);
        }
    }
    return tables;
}

#if defined(GHIDRAENGINE_X86_SIMD)

struct CpuFeatures {
    bool sse2 = false;
    bool avx2 = false;
};

CpuFeatures detect_cpu() noexcept {
    CpuFeatures features;

    auto cpuid = [](int leaf, int subleaf, int regs[4]) noexcept {
#if defined(_MSC_VER)
        __cpuidex(regs, leaf, subleaf);
#else
        __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#endif
    };

    int regs[4] = {0, 0, 0, 0};
    cpuid(0, 0, regs);
    const int max_leaf = regs[0];
    if (max_leaf < 1) {
        return features;
    }

    cpuid(1, 0, regs);
    features.sse2 = (regs[3] & (1 << 26)) != 0;
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx = (regs[2] & (1 << 28)) != 0;
    const bool fma = (regs[2] & (1 << 12)) != 0;

    if (!osxsave || !avx || !fma || max_leaf < 7) {
        return features;
    }

    // The OS must have enabled AVX state, not just the CPU support it: otherwise
    // ymm registers are not preserved across context switches.
    std::uint64_t xcr0 = 0;
#if defined(_MSC_VER)
    xcr0 = _xgetbv(0);
#else
    {
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(low), "=d"(high) : "c"(0));
        xcr0 = (static_cast<std::uint64_t>(high) << 32) | low;
    }
#endif
    if ((xcr0 & 0x6) != 0x6) { // XMM and YMM state
        return features;
    }

    cpuid(7, 0, regs);
    features.avx2 = (regs[1] & (1 << 5)) != 0;
    return features;
}

#endif // GHIDRAENGINE_X86_SIMD

SimdBackend resolve_backend() noexcept {
#if defined(GHIDRAENGINE_X86_SIMD)
    const CpuFeatures features = detect_cpu();
    if (features.avx2) {
        return SimdBackend::Avx2;
    }
    if (features.sse2) {
        return SimdBackend::Sse2;
    }
#elif defined(GHIDRAENGINE_NEON_SIMD)
    return SimdBackend::Neon; // baseline on AArch64
#endif
    return SimdBackend::Scalar;
}

Dct16Fn resolve_kernel(SimdBackend backend) noexcept {
    switch (backend) {
#if defined(GHIDRAENGINE_X86_SIMD)
        case SimdBackend::Avx2:
            return &dct16_avx2;
        case SimdBackend::Sse2:
            return &dct16_sse2;
#endif
#if defined(GHIDRAENGINE_NEON_SIMD)
        case SimdBackend::Neon:
            return &dct16_neon;
#endif
        default:
            break;
    }
    return &dct16_scalar;
}

} // namespace

const DctTables& dct_tables() noexcept {
    static const DctTables tables = build_tables();
    return tables;
}

SimdBackend active_backend() noexcept {
    static const SimdBackend backend = resolve_backend();
    return backend;
}

const char* backend_name(SimdBackend backend) noexcept {
    switch (backend) {
        case SimdBackend::Avx2: return "avx2";
        case SimdBackend::Sse2: return "sse2";
        case SimdBackend::Neon: return "neon";
        case SimdBackend::Scalar: break;
    }
    return "scalar";
}

Dct16Fn dct16() noexcept {
    static const Dct16Fn kernel = resolve_kernel(active_backend());
    return kernel;
}

} // namespace ghidraengine
