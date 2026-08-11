#include "hash/dct.hpp"

#if defined(GHIDRAENGINE_NEON_SIMD)

#include <arm_neon.h>

namespace ghidraengine {

void dct16_neon(const float* input, float* output) noexcept {
    const DctTables& tables = dct_tables();

    alignas(16) float intermediate[kDctOutputSize * kDctInputSize];

    for (std::size_t u = 0; u < kDctOutputSize; ++u) {
        const float* basis = tables.row_basis + u * kDctInputSize;

        float32x4_t acc0 = vdupq_n_f32(0.0F);
        float32x4_t acc1 = vdupq_n_f32(0.0F);
        float32x4_t acc2 = vdupq_n_f32(0.0F);
        float32x4_t acc3 = vdupq_n_f32(0.0F);
        float32x4_t acc4 = vdupq_n_f32(0.0F);
        float32x4_t acc5 = vdupq_n_f32(0.0F);
        float32x4_t acc6 = vdupq_n_f32(0.0F);
        float32x4_t acc7 = vdupq_n_f32(0.0F);

        for (std::size_t y = 0; y < kDctInputSize; ++y) {
            const float weight = basis[y];
            const float* source = input + y * kDctInputSize;
            acc0 = vfmaq_n_f32(acc0, vld1q_f32(source + 0), weight);
            acc1 = vfmaq_n_f32(acc1, vld1q_f32(source + 4), weight);
            acc2 = vfmaq_n_f32(acc2, vld1q_f32(source + 8), weight);
            acc3 = vfmaq_n_f32(acc3, vld1q_f32(source + 12), weight);
            acc4 = vfmaq_n_f32(acc4, vld1q_f32(source + 16), weight);
            acc5 = vfmaq_n_f32(acc5, vld1q_f32(source + 20), weight);
            acc6 = vfmaq_n_f32(acc6, vld1q_f32(source + 24), weight);
            acc7 = vfmaq_n_f32(acc7, vld1q_f32(source + 28), weight);
        }

        float* row = intermediate + u * kDctInputSize;
        vst1q_f32(row + 0, acc0);
        vst1q_f32(row + 4, acc1);
        vst1q_f32(row + 8, acc2);
        vst1q_f32(row + 12, acc3);
        vst1q_f32(row + 16, acc4);
        vst1q_f32(row + 20, acc5);
        vst1q_f32(row + 24, acc6);
        vst1q_f32(row + 28, acc7);
    }

    for (std::size_t v = 0; v < kDctOutputSize; ++v) {
        const float* row = intermediate + v * kDctInputSize;

        float32x4_t acc0 = vdupq_n_f32(0.0F);
        float32x4_t acc1 = vdupq_n_f32(0.0F);
        float32x4_t acc2 = vdupq_n_f32(0.0F);
        float32x4_t acc3 = vdupq_n_f32(0.0F);

        for (std::size_t x = 0; x < kDctInputSize; ++x) {
            const float weight = row[x];
            const float* basis = tables.col_basis + x * kDctOutputSize;
            acc0 = vfmaq_n_f32(acc0, vld1q_f32(basis + 0), weight);
            acc1 = vfmaq_n_f32(acc1, vld1q_f32(basis + 4), weight);
            acc2 = vfmaq_n_f32(acc2, vld1q_f32(basis + 8), weight);
            acc3 = vfmaq_n_f32(acc3, vld1q_f32(basis + 12), weight);
        }

        float* out = output + v * kDctOutputSize;
        vst1q_f32(out + 0, acc0);
        vst1q_f32(out + 4, acc1);
        vst1q_f32(out + 8, acc2);
        vst1q_f32(out + 12, acc3);
    }
}

}

#endif
