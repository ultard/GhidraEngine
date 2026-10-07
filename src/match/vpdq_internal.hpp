#ifndef GHIDRAENGINE_VPDQ_INTERNAL_HPP
#define GHIDRAENGINE_VPDQ_INTERNAL_HPP

#include <GhidraEngine/match/video.hpp>

#include <span>

namespace GhidraEngine::detail {

VpdqComparison compare_vpdq_hashes(
    std::span<const PdqHash> query,
    std::span<const PdqHash> candidate,
    PdqDistance max_distance
);
}
#endif
