#ifndef GHIDRAENGINE_MATCH_IMAGE_HPP
#define GHIDRAENGINE_MATCH_IMAGE_HPP

#include <GhidraEngine/image/fingerprint.hpp>
#include <GhidraEngine/index/pdq_range.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <span>
#include <vector>

namespace GhidraEngine {

struct ImageFingerprintRecord {
    FingerprintId id;
    MediaId media;
    PdqFingerprint fingerprint;
    bool operator==(const ImageFingerprintRecord &) const = default;
};

class GHIDRAENGINE_EXPORT ImageFingerprintCatalog {
public:
    ImageFingerprintCatalog() = default;

    explicit ImageFingerprintCatalog(std::span<const ImageFingerprintRecord> records);

    [[nodiscard]] std::span<const ImageFingerprintRecord> records() const noexcept {
        return records_;
    }

    [[nodiscard]] const ImageFingerprintRecord &at(FingerprintId id) const;

private:
    std::vector<ImageFingerprintRecord> records_;
};

template <PdqRangeIndex Index>
[[nodiscard]] std::vector<FingerprintId>
find_image_candidates(const Index &index, const ImageSignature &query, PdqDistance max_distance) {
    detail::check_image_signature(query);
    std::vector<FingerprintId> candidates;

    for (const auto &variant : query.variants) {
        const auto hits = index.search_within(variant.hash, max_distance.value());

        const auto ids = hits | std::views::transform([](const PdqHit &hit) {
                             return hit.id;
                         });
        std::vector<FingerprintId> merged;
        merged.reserve(std::max(candidates.size(), hits.size()));

        std::ranges::set_union(candidates, ids, std::back_inserter(merged));

        candidates.swap(merged);
    }

    return candidates;
}

[[nodiscard]] GHIDRAENGINE_EXPORT std::vector<ImageMatch> verify_image_candidates(
    MediaId query_id,
    const ImageSignature &query,
    std::span<const FingerprintId> candidates,
    const ImageFingerprintCatalog &catalog,
    const PdqMatchPolicy &policy
);

}
#endif
