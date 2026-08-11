#include "hash/dct.hpp"

namespace ghidraengine {

void dct16_scalar(const float* input, float* output) noexcept {
    const DctTables& tables = dct_tables();

    float intermediate[kDctOutputSize * kDctInputSize];

    for (std::size_t u = 0; u < kDctOutputSize; ++u) {
        const float* basis = tables.row_basis + u * kDctInputSize;
        float* row = intermediate + u * kDctInputSize;

        for (std::size_t x = 0; x < kDctInputSize; ++x) {
            row[x] = 0.0F;
        }
        for (std::size_t y = 0; y < kDctInputSize; ++y) {
            const float weight = basis[y];
            const float* source = input + y * kDctInputSize;
            for (std::size_t x = 0; x < kDctInputSize; ++x) {
                row[x] += weight * source[x];
            }
        }
    }

    for (std::size_t v = 0; v < kDctOutputSize; ++v) {
        const float* row = intermediate + v * kDctInputSize;
        float* out = output + v * kDctOutputSize;

        for (std::size_t u = 0; u < kDctOutputSize; ++u) {
            out[u] = 0.0F;
        }
        for (std::size_t x = 0; x < kDctInputSize; ++x) {
            const float weight = row[x];
            const float* basis = tables.col_basis + x * kDctOutputSize;
            for (std::size_t u = 0; u < kDctOutputSize; ++u) {
                out[u] += weight * basis[u];
            }
        }
    }
}

}
