#ifndef GHIDRAENGINE_MATCH_VIDEO_HPP
#define GHIDRAENGINE_MATCH_VIDEO_HPP

#include <GhidraEngine/core/media.hpp>
#include <GhidraEngine/export.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace GhidraEngine {

struct VpdqMatchPolicy {
    PdqDistance max_distance{31};
    PdqQuality min_quality{50};
    Coverage min_query_coverage{0.8};
    Coverage min_candidate_coverage{0.0};
};

struct VpdqComparison {
    Coverage query_coverage;
    Coverage candidate_coverage;
    std::size_t query_frames{};
    std::size_t candidate_frames{};
    bool operator==(const VpdqComparison &) const = default;
};

[[nodiscard]] GHIDRAENGINE_EXPORT VpdqComparison compare_vpdq(
    const VpdqSignature &query,
    const VpdqSignature &candidate,
    const PdqMatchPolicy &policy
);

[[nodiscard]] GHIDRAENGINE_EXPORT std::optional<VideoMatch> match_vpdq(
    VideoId query_id,
    const VpdqSignature &query,
    VideoId candidate_id,
    const VpdqSignature &candidate,
    const VpdqMatchPolicy &policy = {}
);

namespace detail {

[[nodiscard]] GHIDRAENGINE_EXPORT std::vector<PdqHash>
vpdq_matching_hashes(const VpdqSignature &signature, PdqQuality min_quality);
}

}
#endif
