#include <GhidraEngine/index/flat_pdq.hpp>

#include <GhidraEngine/hash/pdq.hpp>

#include "../cpu/kernels.hpp"

#include <algorithm>
#include <stdexcept>

namespace GhidraEngine {

FlatPdqIndex::FlatPdqIndex(std::span<const PdqIndexEntry> entries)
    : entries_(entries.begin(), entries.end()) {
    std::ranges::sort(entries_, {}, &PdqIndexEntry::id);
    const auto duplicate = std::ranges::adjacent_find(entries_, [](const auto &a, const auto &b) {
        return a.id == b.id;
    });

    if (duplicate != entries_.end()) {
        throw std::invalid_argument("Duplicate FingerprintId in PDQ index");
    }
}

void FlatPdqIndex::insert(const FingerprintId id, const PdqHash &hash) {
    if (entries_.empty() || entries_.back().id < id) {
        entries_.push_back({id, hash});

        return;
    }

    const auto position = std::ranges::lower_bound(entries_, id, {}, &PdqIndexEntry::id);

    if (position != entries_.end() && position->id == id) {
        throw std::invalid_argument("Duplicate FingerprintId in PDQ index");
    }

    entries_.insert(position, {id, hash});
}

std::vector<PdqHit>
FlatPdqIndex::search_within(const PdqHash &query, const std::uint16_t max_distance) const {
    if (max_distance > 256) {
        throw std::invalid_argument("PDQ search distance must be in [0, 256]");
    }

    return detail::search_pdq_entries(entries_, query, max_distance);
}

}
