#ifndef GHIDRAENGINE_HASH_PDQ_INTERNAL_HPP
#define GHIDRAENGINE_HASH_PDQ_INTERNAL_HPP

#include <GhidraEngine/hash/pdq.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace GhidraEngine::detail {

struct PdqWorkspace {
    std::vector<float> input;
    std::vector<float> scratch;
};

GHIDRAENGINE_EXPORT void compute_pdq_variants(
    std::span<const float> luma,
    std::size_t width,
    std::size_t height,
    std::span<PdqFingerprint> output,
    PdqWorkspace &workspace
);

void compute_pdq_variants(
    std::span<const float> luma,
    std::size_t width,
    std::size_t height,
    std::span<PdqFingerprint> output
);

}

#endif
