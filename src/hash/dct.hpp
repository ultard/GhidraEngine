// Only the top-left 16x16 of the 32x32 transform is used, so the kernels compute
// exactly that: 24576 multiply-accumulates instead of 65536, a 2.7x reduction
// before any SIMD is involved.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ghidraengine {

inline constexpr std::size_t kDctInputSize = 32;   // 32x32 input samples
inline constexpr std::size_t kDctOutputSize = 16;  // 16x16 retained coefficients
inline constexpr std::size_t kDctInputCount = kDctInputSize * kDctInputSize;
inline constexpr std::size_t kDctOutputCount = kDctOutputSize * kDctOutputSize;

// DCT-II basis, split for the two passes. col_basis is the transpose of
// row_basis, materialised so the column pass walks contiguous memory.
//   row_basis[u * 32 + y] = cos((2y+1) * u * pi / 64)   (16 x 32, row-major)
//   col_basis[x * 16 + v] = cos((2x+1) * v * pi / 64)   (32 x 16, row-major)
struct DctTables {
    alignas(64) float row_basis[kDctOutputSize * kDctInputSize];
    alignas(64) float col_basis[kDctInputSize * kDctOutputSize];
};

const DctTables& dct_tables() noexcept;

// input:  kDctInputCount floats, row-major 32x32
// output: kDctOutputCount floats, row-major 16x16, output[v * 16 + u]
using Dct16Fn = void (*)(const float* input, float* output) noexcept;

// Reference the SIMD kernels are validated against, and the fallback.
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

// Both resolved once on first use from the running CPU's feature flags.
SimdBackend active_backend() noexcept;
const char* backend_name(SimdBackend backend) noexcept;
Dct16Fn dct16() noexcept;

} // namespace ghidraengine
