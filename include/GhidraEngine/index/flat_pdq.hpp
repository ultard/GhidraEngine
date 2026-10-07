#ifndef GHIDRAENGINE_INDEX_FLAT_PDQ_HPP
#define GHIDRAENGINE_INDEX_FLAT_PDQ_HPP

#include <GhidraEngine/export.hpp>
#include <GhidraEngine/index/pdq_range.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace GhidraEngine {

class GHIDRAENGINE_EXPORT FlatPdqIndex {
public:
    FlatPdqIndex() = default;

    explicit FlatPdqIndex(std::span<const PdqIndexEntry> entries);

    void insert(FingerprintId id, const PdqHash &hash);

    [[nodiscard]] std::vector<PdqHit>
    search_within(const PdqHash &query, std::uint16_t max_distance) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    void reserve(const std::size_t capacity) {
        entries_.reserve(capacity);
    }

private:
    friend class MihPdqIndex;
    std::vector<PdqIndexEntry> entries_;
};

}

#endif
