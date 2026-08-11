#pragma once

#include <cstddef>
#include <cstdint>

namespace ghidraengine {

inline constexpr std::size_t kDctInputSize = 32;
inline constexpr std::size_t kDctOutputSize = 16;
inline constexpr std::size_t kDctInputCount = kDctInputSize * kDctInputSize;
inline constexpr std::size_t kDctOutputCount = kDctOutputSize * kDctOutputSize;

struct DctTables {
    alignas(64) float row_basis[kDctOutputSize * kDctInputSize];
    alignas(64) float col_basis[kDctInputSize * kDctOutputSize];
};

const DctTables& dct_tables() noexcept;

using Dct16Fn = void (*)(const float* input, float* output) noexcept;

void dct16_scalar(const float* input, float* output) noexcept;

#if defined(GHIDRAENGINE_X86_SIMD)
void dct16_sse2(const float* input, float* output) noexcept;
void dct16_avx2(const float* input, float* output) noexcept;
#endif
#if defined(GHIDRAENGINE_NEON_SIMD)
void dct16_neon(const float* input, float* output) noexcept;
#endif

enum class SimdBackend : std::uint8_t {
    Scalar = 0,
    Sse2,
    Avx2,
    Neon,
};

SimdBackend active_backend() noexcept;
const char* backend_name(SimdBackend backend) noexcept;
Dct16Fn dct16() noexcept;

}
