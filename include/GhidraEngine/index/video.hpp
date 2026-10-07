#ifndef GHIDRAENGINE_INDEX_VIDEO_HPP
#define GHIDRAENGINE_INDEX_VIDEO_HPP

#include <GhidraEngine/index/pdq_range.hpp>
#include <GhidraEngine/match/video.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace GhidraEngine {

struct VideoSignatureRecord {
    VideoId id;
    VpdqSignature signature;
    bool operator==(const VideoSignatureRecord &) const = default;
};

struct VideoFramePayload {
    FingerprintId id;
    VideoId video;
    Timestamp timestamp;
    PdqQuality quality;
    bool operator==(const VideoFramePayload &) const = default;
};

class GHIDRAENGINE_EXPORT VideoFingerprintCatalog {
public:
    VideoFingerprintCatalog() = default;

    explicit VideoFingerprintCatalog(std::span<const VideoSignatureRecord> records);

    VideoFingerprintCatalog(const VideoFingerprintCatalog &) = default;

    VideoFingerprintCatalog(VideoFingerprintCatalog &&other) noexcept;

    VideoFingerprintCatalog &operator=(VideoFingerprintCatalog other) noexcept;

    [[nodiscard]] std::span<const VideoSignatureRecord> records() const noexcept {
        return records_;
    }

    [[nodiscard]] std::span<const VideoFramePayload> frames() const noexcept {
        return frames_;
    }

    [[nodiscard]] std::vector<PdqIndexEntry> index_entries() const;

    [[nodiscard]] const VpdqSignature &at(VideoId id) const;

    [[nodiscard]] const VideoFramePayload &frame_at(FingerprintId id) const;

private:
    void swap(VideoFingerprintCatalog &other) noexcept;
    std::vector<VideoSignatureRecord> records_;
    std::vector<VideoFramePayload> frames_;
};

template <PdqRangeIndex Index>
[[nodiscard]] std::vector<VideoId> find_video_candidates(
    const Index &index,
    const VideoFingerprintCatalog &catalog,
    const VpdqSignature &query,
    const VpdqMatchPolicy &policy = {}
) {
    std::vector<VideoId> candidates;

    if (policy.min_query_coverage.value() == 0.0 && policy.min_candidate_coverage.value() == 0.0) {
        for (const auto &record : catalog.records()) {
            candidates.push_back(record.id);
        }

        return candidates;
    }

    const auto hashes = detail::vpdq_matching_hashes(query, policy.min_quality);
    std::unordered_set<std::uint64_t> seen;

    for (const auto &hash : hashes) {
        for (const auto &hit : index.search_within(hash, policy.max_distance.value())) {
            const auto &payload = catalog.frame_at(hit.id);

            if (payload.quality >= policy.min_quality && seen.insert(payload.video.value).second) {
                candidates.push_back(payload.video);
            }
        }
    }

    std::ranges::sort(candidates);

    return candidates;
}

[[nodiscard]] GHIDRAENGINE_EXPORT std::vector<VideoMatch> verify_video_candidates(
    VideoId query_id,
    const VpdqSignature &query,
    std::span<const VideoId> candidates,
    const VideoFingerprintCatalog &catalog,
    const VpdqMatchPolicy &policy = {}
);

}
#endif
