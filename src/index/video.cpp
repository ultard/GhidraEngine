#include <GhidraEngine/index/video.hpp>

#include "../match/vpdq_internal.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace GhidraEngine {

VideoFingerprintCatalog::VideoFingerprintCatalog(VideoFingerprintCatalog &&other) noexcept {
    swap(other);
}

VideoFingerprintCatalog &
VideoFingerprintCatalog::operator=(VideoFingerprintCatalog other) noexcept {
    swap(other);

    return *this;
}

void VideoFingerprintCatalog::swap(VideoFingerprintCatalog &other) noexcept {
    records_.swap(other.records_);
    frames_.swap(other.frames_);
}

VideoFingerprintCatalog::VideoFingerprintCatalog(std::span<const VideoSignatureRecord> records)
    : records_(records.begin(), records.end()) {
    std::ranges::sort(records_, {}, &VideoSignatureRecord::id);
    const auto duplicate = std::ranges::adjacent_find(records_, [](const auto &a, const auto &b) {
        return a.id == b.id;
    });

    if (duplicate != records_.end()) {
        throw std::invalid_argument("Duplicate VideoId in video catalog");
    }

    for (const auto &record : records_) {
        for (const auto &frame : record.signature.frames) {
            if (frames_.size() == std::numeric_limits<std::uint64_t>::max()) {
                throw std::length_error("Video frame IDs exhausted");
            }

            frames_.push_back(
                {FingerprintId{(frames_.size())},
                 record.id,
                 frame.timestamp,
                 frame.quality}
            );
        }
    }
}

std::vector<PdqIndexEntry> VideoFingerprintCatalog::index_entries() const {
    std::vector<PdqIndexEntry> entries;
    entries.reserve(frames_.size());

    for (const auto &record : records_) {
        for (const auto &frame : record.signature.frames) {
            entries.push_back(
                {FingerprintId{(entries.size())}, frame.hash}
            );
        }
    }

    return entries;
}

const VpdqSignature &VideoFingerprintCatalog::at(VideoId id) const {
    const auto found = std::ranges::lower_bound(records_, id, {}, &VideoSignatureRecord::id);

    if (found == records_.end() || found->id != id) {
        throw std::out_of_range("Missing VideoId " + std::to_string(id.value));
    }

    return found->signature;
}

const VideoFramePayload &VideoFingerprintCatalog::frame_at(FingerprintId id) const {
    if (id.value >= frames_.size()) {
        throw std::out_of_range("Missing video FingerprintId " + std::to_string(id.value));
    }

    return frames_[id.value];
}

std::vector<VideoMatch> verify_video_candidates(
    const VideoId query_id,
    const VpdqSignature &query,
    std::span<const VideoId> candidates,
    const VideoFingerprintCatalog &catalog,
    const VpdqMatchPolicy &policy
) {
    std::vector ids(candidates.begin(), candidates.end());
    std::ranges::sort(ids);
    ids.erase(std::ranges::unique(ids).begin(), ids.end());
    std::vector<VideoMatch> matches;
    std::optional<std::vector<PdqHash>> queries;

    for (const auto id : ids) {
        const auto &signature = catalog.at(id);

        if (query_id == id) {
            continue;
        }

        if (!queries) {
            queries = detail::vpdq_matching_hashes(query, policy.min_quality);
        }

        const auto hashes = detail::vpdq_matching_hashes(signature, policy.min_quality);
        const auto comparison = detail::compare_vpdq_hashes(*queries, hashes, policy.max_distance);

        if (comparison.query_frames == 0 || comparison.candidate_frames == 0) {
            continue;
        }

        if (comparison.query_coverage.value() < policy.min_query_coverage.value() ||
            comparison.candidate_coverage.value() < policy.min_candidate_coverage.value()) {
            continue;
        }

        matches.push_back({query_id, id, comparison.query_coverage, comparison.candidate_coverage});
    }

    return matches;
}
}
