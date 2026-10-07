#include <GhidraEngine/match/video.hpp>

#include <GhidraEngine/hash/pdq.hpp>

#include "vpdq_byte_index.hpp"
#include "vpdq_internal.hpp"

#include <algorithm>

namespace GhidraEngine {

std::vector<PdqHash>
detail::vpdq_matching_hashes(const VpdqSignature &signature, const PdqQuality min_quality) {
    std::vector<std::size_t> order(signature.frames.size());

    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }

    std::ranges::sort(order, [&](auto a, auto b) {
        const auto comparison = signature.frames[a].hash <=> signature.frames[b].hash;

        return comparison < 0 || (comparison == 0 && a < b);
    });
    std::vector<PdqHash> hashes;
    std::optional<PdqHash> previous;

    for (const auto position : order) {
        const auto &frame = signature.frames[position];

        if (previous && *previous == frame.hash) {
            continue;
        }

        if (frame.quality >= min_quality) {
            hashes.push_back(frame.hash);
        }

        previous = frame.hash;
    }

    return hashes;
}

VpdqComparison detail::compare_vpdq_hashes(
    std::span<const PdqHash> queries,
    std::span<const PdqHash> candidates,
    PdqDistance max_distance
) {
    VpdqComparison result{{}, {}, queries.size(), candidates.size()};

    if (queries.empty() || candidates.empty()) {
        return result;
    }

    if (max_distance.value() == 256) {
        result.query_coverage = Coverage{1.0};
        result.candidate_coverage = Coverage{1.0};

        return result;
    }

    if (max_distance.value() == 0) {
        std::size_t common = 0;
        auto q = queries.begin();
        auto c = candidates.begin();

        while (q != queries.end() && c != candidates.end()) {
            if (*q < *c) {
                ++q;
            } else if (*c < *q) {
                ++c;
            } else {
                ++common;
                ++q;
                ++c;
            }
        }

        result.query_coverage =
            Coverage{static_cast<double>(common) / static_cast<double>(queries.size())};
        result.candidate_coverage =
            Coverage{static_cast<double>(common) / static_cast<double>(candidates.size())};

        return result;
    }

    const auto coverage = [&](const auto &from, const auto &to) {
        const auto count = detail::vpdq_coverage_count(from, to, max_distance.value()).matches;

        return Coverage{static_cast<double>(count) / static_cast<double>(from.size())};
    };
    result.query_coverage = coverage(queries, candidates);
    result.candidate_coverage = coverage(candidates, queries);

    return result;
}

VpdqComparison compare_vpdq(
    const VpdqSignature &query,
    const VpdqSignature &candidate,
    const PdqMatchPolicy &policy
) {
    const auto queries = detail::vpdq_matching_hashes(query, policy.min_quality);
    const auto candidates = detail::vpdq_matching_hashes(candidate, policy.min_quality);

    return detail::compare_vpdq_hashes(queries, candidates, policy.max_distance);
}

std::optional<VideoMatch> match_vpdq(
    const VideoId query_id,
    const VpdqSignature &query,
    const VideoId candidate_id,
    const VpdqSignature &candidate,
    const VpdqMatchPolicy &policy
) {
    if (query_id == candidate_id) {
        return std::nullopt;
    }

    const auto comparison =
        compare_vpdq(query, candidate, {policy.max_distance, policy.min_quality});

    if (comparison.query_frames == 0 || comparison.candidate_frames == 0) {
        return std::nullopt;
    }

    if (comparison.query_coverage.value() < policy.min_query_coverage.value() ||
        comparison.candidate_coverage.value() < policy.min_candidate_coverage.value()) {
        return std::nullopt;
    }

    return VideoMatch{
        query_id,
        candidate_id,
        comparison.query_coverage,
        comparison.candidate_coverage
    };
}
}
