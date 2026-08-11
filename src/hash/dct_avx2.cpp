// Own translation unit with -mavx2 / arch:AVX2; reached only after cpuid confirms
// the ISA and the OS has enabled ymm state.
#include "hash/dct.hpp"

#if defined(GHIDRAENGINE_X86_SIMD)

#include <immintrin.h>

namespace ghidraengine {

void dct16_avx2(const float* input, float* output) noexcept {
    const DctTables& tables = dct_tables();

    alignas(32) float intermediate[kDctOutputSize * kDctInputSize];

    // Four ymm accumulators held across the whole reduction over y, so the
    // intermediate row is written once, not 32 times.
    for (std::size_t u = 0; u < kDctOutputSize; ++u) {
        const float* basis = tables.row_basis + u * kDctInputSize;

        __m256 acc0 = _mm256_setzero_ps();
        __m256 acc1 = _mm256_setzero_ps();
        __m256 acc2 = _mm256_setzero_ps();
        __m256 acc3 = _mm256_setzero_ps();

        for (std::size_t y = 0; y < kDctInputSize; ++y) {
            const __m256 weight = _mm256_broadcast_ss(basis + y);
            const float* source = input + y * kDctInputSize;
            acc0 = _mm256_fmadd_ps(weight, _mm256_loadu_ps(source + 0), acc0);
            acc1 = _mm256_fmadd_ps(weight, _mm256_loadu_ps(source + 8), acc1);
            acc2 = _mm256_fmadd_ps(weight, _mm256_loadu_ps(source + 16), acc2);
            acc3 = _mm256_fmadd_ps(weight, _mm256_loadu_ps(source + 24), acc3);
        }

        float* row = intermediate + u * kDctInputSize;
        _mm256_store_ps(row + 0, acc0);
        _mm256_store_ps(row + 8, acc1);
        _mm256_store_ps(row + 16, acc2);
        _mm256_store_ps(row + 24, acc3);
    }

    // Accumulating along x keeps the 16 outputs in two ymm registers and avoids
    // the 256 horizontal reductions a dot-product formulation would cost.
    for (std::size_t v = 0; v < kDctOutputSize; ++v) {
        const float* row = intermediate + v * kDctInputSize;

        __m256 acc0 = _mm256_setzero_ps();
        __m256 acc1 = _mm256_setzero_ps();

        for (std::size_t x = 0; x < kDctInputSize; ++x) {
            const __m256 weight = _mm256_broadcast_ss(row + x);
            const float* basis = tables.col_basis + x * kDctOutputSize;
            acc0 = _mm256_fmadd_ps(weight, _mm256_loadu_ps(basis + 0), acc0);
            acc1 = _mm256_fmadd_ps(weight, _mm256_loadu_ps(basis + 8), acc1);
        }

        float* out = output + v * kDctOutputSize;
        _mm256_storeu_ps(out + 0, acc0);
        _mm256_storeu_ps(out + 8, acc1);
    }

    _mm256_zeroupper(); // avoid the AVX-SSE transition penalty on the way out
}

} // namespace ghidraengine

#endif // GHIDRAENGINE_X86_SIMD
