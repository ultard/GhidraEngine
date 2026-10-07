#include <GhidraEngine/match/image.hpp>

#include "../cpu/kernels.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace GhidraEngine {

ImageFingerprintCatalog::ImageFingerprintCatalog(std::span<const ImageFingerprintRecord> records)
    : records_(records.begin(), records.end()) {
    std::ranges::sort(records_, {}, &ImageFingerprintRecord::id);
    const auto duplicate = std::ranges::adjacent_find(records_, [](const auto &a, const auto &b) {
        return a.id == b.id;
    });

    if (duplicate != records_.end()) {
        throw std::invalid_argument("Duplicate FingerprintId in image catalog");
    }
}

const ImageFingerprintRecord &ImageFingerprintCatalog::at(FingerprintId id) const {
    const auto found = std::ranges::lower_bound(records_, id, {}, &ImageFingerprintRecord::id);

    if (found == records_.end() || found->id != id) {
        throw std::out_of_range("Missing image FingerprintId " + std::to_string(id.value));
    }

    return *found;
}

std::vector<ImageMatch> verify_image_candidates(
    MediaId query_id,
    const ImageSignature &query,
    std::span<const FingerprintId> candidates,
    const ImageFingerprintCatalog &catalog,
    const PdqMatchPolicy &policy
) {
    detail::check_image_signature(query);
    std::vector<FingerprintId> normalized;
    auto ids = candidates;

    if (!std::ranges::is_sorted(ids) || std::ranges::adjacent_find(ids) != ids.end()) {
        normalized.assign(ids.begin(), ids.end());
        std::ranges::sort(normalized);
        normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
        ids = normalized;
    }

    std::vector<ImageMatch> matches;
    const auto bounded = detail::cpu_kernels().bounded;

    for (const auto id : ids) {
        const auto &record = catalog.at(id);

        if (record.media == query_id || record.fingerprint.quality < policy.min_quality) {
            continue;
        }

        auto best = PdqDistance{256};
        bool accepted = false;

        for (const auto &variant : query.variants) {
            if (variant.quality < policy.min_quality) {
                continue;
            }

            const auto distance = bounded(
                variant.hash,
                record.fingerprint.hash,
                policy.max_distance.value()
            );

            if (distance > policy.max_distance.value()) {
                continue;
            }

            best = std::min(best, PdqDistance{distance});
            accepted = true;
        }

        if (!accepted) {
            continue;
        }

        matches.push_back({query_id, record.media, best});
    }

    std::ranges::sort(matches, [](const auto &a, const auto &b) {
        if (a.candidate == b.candidate) {
            return a.distance < b.distance;
        }

        return a.candidate < b.candidate;
    });
    matches.erase(
        std::unique(
            matches.begin(),
            matches.end(),
            [](const auto &a, const auto &b) {
                return a.candidate == b.candidate;
            }
        ),
        matches.end()
    );

    return matches;
}

}
