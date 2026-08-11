// Fallback for pre-Haswell x86, where SSE2 is guaranteed by the x86-64 baseline.
// Same loop structure as the AVX2 path at half the width and without FMA.
#include "hash/dct.hpp"

#if defined(GHIDRAENGINE_X86_SIMD)

#include <emmintrin.h>

namespace ghidraengine {

void dct16_sse2(const float* input, float* output) noexcept {
    const DctTables& tables = dct_tables();

    alignas(16) float intermediate[kDctOutputSize * kDctInputSize];

    for (std::size_t u = 0; u < kDctOutputSize; ++u) {
        const float* basis = tables.row_basis + u * kDctInputSize;

        __m128 acc0 = _mm_setzero_ps();
        __m128 acc1 = _mm_setzero_ps();
        __m128 acc2 = _mm_setzero_ps();
        __m128 acc3 = _mm_setzero_ps();
        __m128 acc4 = _mm_setzero_ps();
        __m128 acc5 = _mm_setzero_ps();
        __m128 acc6 = _mm_setzero_ps();
        __m128 acc7 = _mm_setzero_ps();

        for (std::size_t y = 0; y < kDctInputSize; ++y) {
            const __m128 weight = _mm_set1_ps(basis[y]);
            const float* source = input + y * kDctInputSize;
            acc0 = _mm_add_ps(acc0, _mm_mul_ps(weight, _mm_loadu_ps(source + 0)));
            acc1 = _mm_add_ps(acc1, _mm_mul_ps(weight, _mm_loadu_ps(source + 4)));
            acc2 = _mm_add_ps(acc2, _mm_mul_ps(weight, _mm_loadu_ps(source + 8)));
            acc3 = _mm_add_ps(acc3, _mm_mul_ps(weight, _mm_loadu_ps(source + 12)));
            acc4 = _mm_add_ps(acc4, _mm_mul_ps(weight, _mm_loadu_ps(source + 16)));
            acc5 = _mm_add_ps(acc5, _mm_mul_ps(weight, _mm_loadu_ps(source + 20)));
            acc6 = _mm_add_ps(acc6, _mm_mul_ps(weight, _mm_loadu_ps(source + 24)));
            acc7 = _mm_add_ps(acc7, _mm_mul_ps(weight, _mm_loadu_ps(source + 28)));
        }

        float* row = intermediate + u * kDctInputSize;
        _mm_store_ps(row + 0, acc0);
        _mm_store_ps(row + 4, acc1);
        _mm_store_ps(row + 8, acc2);
        _mm_store_ps(row + 12, acc3);
        _mm_store_ps(row + 16, acc4);
        _mm_store_ps(row + 20, acc5);
        _mm_store_ps(row + 24, acc6);
        _mm_store_ps(row + 28, acc7);
    }

    for (std::size_t v = 0; v < kDctOutputSize; ++v) {
        const float* row = intermediate + v * kDctInputSize;

        __m128 acc0 = _mm_setzero_ps();
        __m128 acc1 = _mm_setzero_ps();
        __m128 acc2 = _mm_setzero_ps();
        __m128 acc3 = _mm_setzero_ps();

        for (std::size_t x = 0; x < kDctInputSize; ++x) {
            const __m128 weight = _mm_set1_ps(row[x]);
            const float* basis = tables.col_basis + x * kDctOutputSize;
            acc0 = _mm_add_ps(acc0, _mm_mul_ps(weight, _mm_loadu_ps(basis + 0)));
            acc1 = _mm_add_ps(acc1, _mm_mul_ps(weight, _mm_loadu_ps(basis + 4)));
            acc2 = _mm_add_ps(acc2, _mm_mul_ps(weight, _mm_loadu_ps(basis + 8)));
            acc3 = _mm_add_ps(acc3, _mm_mul_ps(weight, _mm_loadu_ps(basis + 12)));
        }

        float* out = output + v * kDctOutputSize;
        _mm_storeu_ps(out + 0, acc0);
        _mm_storeu_ps(out + 4, acc1);
        _mm_storeu_ps(out + 8, acc2);
        _mm_storeu_ps(out + 12, acc3);
    }
}

} // namespace ghidraengine

#endif // GHIDRAENGINE_X86_SIMD
