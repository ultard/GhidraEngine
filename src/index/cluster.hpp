#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "ghidraengine/config.hpp"
#include "ghidraengine/types.hpp"

namespace ghidraengine {

struct MatchPair {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t distance = 0;
};

class UnionFind {
public:
    explicit UnionFind(std::size_t count);

    std::uint32_t find(std::uint32_t item);
    void unite(std::uint32_t a, std::uint32_t b);

private:
    std::vector<std::uint32_t> parent_;
    std::vector<std::uint32_t> rank_;
};

// High recall, but A~B and B~C merge even when A and C are visibly different,
// chaining runs of gradually-varying images into one cluster.
std::vector<std::vector<std::uint32_t>> group_transitive(std::size_t item_count,
                                                         std::span<const MatchPair> pairs);

// Each cluster is seeded by an unassigned item and admits only direct matches, so
// no chain can form. Costs a few extra clusters when a genuine group straddles
// the threshold — the right trade when a false merge deletes the wrong file.
std::vector<std::vector<std::uint32_t>> group_strict(std::size_t item_count,
                                                     std::span<const MatchPair> pairs);

// Precomputed so the keeper comparison is a plain field lookup.
struct KeeperInfo {
    std::uint64_t pixels = 0; // width * height; 0 when unknown
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::size_t path_length = 0;
};

// Ties fall through to size, then shortest path, so the answer is deterministic.
std::uint32_t choose_keeper(std::span<const std::uint32_t> members,
                            const std::vector<KeeperInfo>& info, KeeperPolicy policy);

} // namespace ghidraengine
