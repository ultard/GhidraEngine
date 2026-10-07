#ifndef GHIDRAENGINE_INDEX_PDQ_RANGE_HPP
#define GHIDRAENGINE_INDEX_PDQ_RANGE_HPP

#include <GhidraEngine/core/fingerprints.hpp>

#include <concepts>
#include <cstdint>
#include <vector>

namespace GhidraEngine {

struct PdqIndexEntry {
    FingerprintId id;
    PdqHash hash;
    bool operator==(const PdqIndexEntry &) const = default;
};

template <class Index>
concept PdqRangeIndex = requires(
    Index &index,
    const Index &read_only,
    FingerprintId id,
    const PdqHash &hash,
    std::uint16_t threshold
) {
    { index.insert(id, hash) } -> std::same_as<void>;

    { read_only.search_within(hash, threshold) } -> std::same_as<std::vector<PdqHit>>;
};

}
#endif
