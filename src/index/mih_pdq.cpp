#include <GhidraEngine/index/mih_pdq.hpp>

#include <GhidraEngine/hash/pdq.hpp>

#include "../cpu/kernels.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace GhidraEngine {

namespace {

constexpr auto end = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t buckets_per_slot = 1U << 16U;
constexpr std::size_t bucket_count = 16 * buckets_per_slot;

std::size_t bucket(const PdqHash &hash, const std::size_t slot) noexcept {
    const auto key = (hash.words[slot / 4] >> (16 * (slot % 4))) & 0xffffU;

    return slot * buckets_per_slot + static_cast<std::size_t>(key);
}

constexpr auto masks = [] {
    std::array<std::uint16_t, 137> result{};
    std::size_t offset = 1;

    for (unsigned i = 0; i < 16; ++i) {
        result[offset++] = static_cast<std::uint16_t>(1U << i);
    }

    for (unsigned i = 0; i < 16; ++i) {
        for (unsigned j = i + 1; j < 16; ++j) {
            result[offset++] = static_cast<std::uint16_t>((1U << i) | (1U << j));
        }
    }

    return result;
}();
constexpr std::array<std::size_t, 4> ball_sizes{0, 1, 17, 137};

void validate_size(const std::size_t size) {
    if (size > end) {
        throw std::length_error("MIH supports at most UINT32_MAX records");
    }
}

}

MihPdqIndex::MihPdqIndex(const std::span<const PdqIndexEntry> entries) {
    validate_size(entries.size());

    if (entries.empty()) {
        return;
    }

    entries_.assign(entries.begin(), entries.end());

    if (std::ranges::is_sorted(entries_, {}, &PdqIndexEntry::id)) {
        const auto duplicate = std::ranges::adjacent_find(
            entries_,
            [](const auto &a, const auto &b) {
                return a.id == b.id;
            }
        );

        if (duplicate != entries_.end()) {
            throw std::invalid_argument("Duplicate FingerprintId in PDQ index");
        }
    } else {
        ids_.reserve(entries.size());

        for (const auto &entry : entries) {
            if (!ids_.insert(entry.id.value).second) {
                throw std::invalid_argument("Duplicate FingerprintId in PDQ index");
            }
        }
    }

    links_.resize(entries.size());
    allocate_tables();

    for (std::size_t i = 0; i < size(); ++i) {
        link(i);
    }
}

MihPdqIndex::MihPdqIndex(MihPdqIndex &&other) noexcept {
    swap(other);
}

MihPdqIndex &MihPdqIndex::operator=(MihPdqIndex other) noexcept {
    swap(other);

    return *this;
}

void MihPdqIndex::swap(MihPdqIndex &other) noexcept {
    entries_.swap(other.entries_);
    ids_.swap(other.ids_);
    links_.swap(other.links_);
    heads_.swap(other.heads_);
    counts_.swap(other.counts_);
}

void MihPdqIndex::allocate_tables() {
    if (!heads_.empty()) {
        return;
    }

    std::vector heads(bucket_count, end);
    std::vector<std::uint32_t> counts(bucket_count, 0);
    heads_.swap(heads);
    counts_.swap(counts);
}

void MihPdqIndex::reserve(std::size_t capacity) {
    validate_size(capacity);
    entries_.reserve(capacity);
    links_.reserve(capacity);

    if (!ids_.empty()) {
        ids_.reserve(capacity);
    }
}

void MihPdqIndex::link(std::size_t position) noexcept {
    for (std::size_t slot = 0; slot < 16; ++slot) {
        const auto key = bucket(entries_[position].hash, slot);
        links_[position][slot] = heads_[key];
        heads_[key] = static_cast<std::uint32_t>(position);
        ++counts_[key];
    }
}

void MihPdqIndex::insert(const FingerprintId id, const PdqHash &hash) {
    // An empty ID set means records are sorted; increasing IDs need no hash allocation.
    const bool unordered = !ids_.empty();
    const bool out_of_order = !entries_.empty() && id <= entries_.back().id;

    if (unordered && ids_.contains(id.value)) {
        throw std::invalid_argument("Duplicate FingerprintId in PDQ index");
    }

    if (!unordered && out_of_order) {
        const auto position = std::ranges::lower_bound(entries_, id, {}, &PdqIndexEntry::id);

        if (position != entries_.end() && position->id == id) {
            throw std::invalid_argument("Duplicate FingerprintId in PDQ index");
        }
    }

    if (size() == end) {
        throw std::length_error("MIH supports at most UINT32_MAX records");
    }

    if (size() == entries_.capacity() || size() == links_.capacity()) {
        const auto growth = std::min(size() / 2 + 1, static_cast<std::size_t>(end) - size());
        reserve(size() + growth);
    }

    allocate_tables();

    if (unordered) {
        ids_.insert(id.value);
    } else if (out_of_order) {
        // Build separately so allocation failure leaves sorted records and their lookup intact.
        std::unordered_set<std::uint64_t> ids;
        ids.reserve(entries_.capacity());

        for (const auto &entry : entries_) {
            ids.insert(entry.id.value);
        }

        ids.insert(id.value);
        ids_.swap(ids);
    }

    entries_.push_back({id, hash});
    links_.push_back(Links{});
    link(size() - 1);
}

std::size_t MihPdqIndex::storage_bytes() const noexcept {
    // The standard library does not expose unordered_set node allocation sizes.
    const auto id_bytes = ids_.empty() ? 0 :
        ids_.bucket_count() * sizeof(void *) +
            ids_.size() * (sizeof(std::uint64_t) + 2 * sizeof(void *));

    return entries_.capacity() * sizeof(PdqIndexEntry) +
           links_.capacity() * sizeof(Links) + id_bytes +
           (heads_.capacity() + counts_.capacity()) * sizeof(std::uint32_t);
}

std::vector<PdqHit> MihPdqIndex::search_within(
    const PdqHash &query,
    const std::uint16_t max_distance,
    MihSearchStats *stats
) const {
    if (max_distance > 256) {
        throw std::invalid_argument("PDQ search distance must be in [0, 256]");
    }

    MihSearchStats measured;
    const auto fallback = [&] {
        auto hits = detail::search_pdq_entries(entries_, query, max_distance);

        if (!ids_.empty()) {
            std::ranges::sort(hits, {}, &PdqHit::id);
        }

        measured.used_flat = true;
        measured.candidates_verified = size();

        if (stats != nullptr) {
            *stats = measured;
        }

        return hits;
    };

    if (max_distance >= 48 || size() == 0) {
        return fallback();
    }

    const auto radius = static_cast<std::size_t>(max_distance / 16);
    const auto wider_slots = static_cast<std::size_t>(max_distance % 16) + 1;
    const auto lookups =
        wider_slots * ball_sizes[radius + 1] + (16 - wider_slots) * ball_sizes[radius];

    const auto budget = size() / 64;

    if (lookups >= budget) {
        return fallback();
    }

    std::vector<std::uint32_t> selected;
    selected.reserve(lookups);
    std::size_t postings = 0;

    for (std::size_t slot = 0; slot < 16; ++slot) {
        const auto mask_count = ball_sizes[radius + (slot < wider_slots ? 1 : 0)];
        const auto base = bucket(query, slot);

        for (std::size_t i = 0; i < mask_count; ++i) {
            const auto key = base ^ masks[i];
            ++measured.slot_lookups;

            if (counts_[key] >= budget - lookups - postings) {
                return fallback();
            }

            postings += counts_[key];
            selected.push_back(static_cast<std::uint32_t>(key));
        }
    }

    std::vector<std::uint32_t> candidates;
    candidates.reserve(postings);

    for (const auto key : selected) {
        const auto slot = key / buckets_per_slot;

        for (auto position = heads_[key]; position != end; position = links_[position][slot]) {
            candidates.push_back(position);
        }
    }

    measured.posting_visits = candidates.size();
    std::ranges::sort(candidates);
    candidates.erase(std::ranges::unique(candidates).begin(), candidates.end());
    measured.candidates_verified = candidates.size();
    std::vector<PdqHit> hits;
    const auto bounded = detail::cpu_kernels().bounded;

    for (const auto position : candidates) {
        const auto &entry = entries_[position];
        const auto distance = bounded(query, entry.hash, max_distance);

        if (distance <= max_distance) {
            hits.push_back({entry.id, PdqDistance{distance}});
        }
    }

    if (!ids_.empty()) {
        std::ranges::sort(hits, {}, &PdqHit::id);
    }

    if (stats != nullptr) {
        *stats = measured;
    }

    return hits;
}

}
