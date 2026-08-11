#include "index/cluster.hpp"

#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace ghidraengine {

UnionFind::UnionFind(std::size_t count) : parent_(count), rank_(count, 0) {
    std::iota(parent_.begin(), parent_.end(), 0U);
}

std::uint32_t UnionFind::find(std::uint32_t item) {
    // Path halving: same asymptotics as full compression without the second pass.
    while (parent_[item] != item) {
        parent_[item] = parent_[parent_[item]];
        item = parent_[item];
    }
    return item;
}

void UnionFind::unite(std::uint32_t a, std::uint32_t b) {
    a = find(a);
    b = find(b);
    if (a == b) {
        return;
    }
    if (rank_[a] < rank_[b]) {
        std::swap(a, b);
    }
    parent_[b] = a;
    if (rank_[a] == rank_[b]) {
        ++rank_[a];
    }
}

std::vector<std::vector<std::uint32_t>> group_transitive(std::size_t item_count,
                                                         std::span<const MatchPair> pairs) {
    UnionFind sets(item_count);
    for (const MatchPair& pair : pairs) {
        sets.unite(pair.a, pair.b);
    }

    // Membership is derived from the pair list rather than by scanning every item,
    // so an isolated file never allocates a group.
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> groups;
    std::vector<bool> touched(item_count, false);
    for (const MatchPair& pair : pairs) {
        for (const std::uint32_t item : {pair.a, pair.b}) {
            if (!touched[item]) {
                touched[item] = true;
                groups[sets.find(item)].push_back(item);
            }
        }
    }

    std::vector<std::vector<std::uint32_t>> result;
    result.reserve(groups.size());
    for (auto& [root, members] : groups) {
        if (members.size() < 2) {
            continue;
        }
        std::sort(members.begin(), members.end());
        result.push_back(std::move(members));
    }
    // Deterministic output order regardless of hash-map iteration order.
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.front() < b.front(); });
    return result;
}

std::vector<std::vector<std::uint32_t>> group_strict(std::size_t item_count,
                                                     std::span<const MatchPair> pairs) {
    // Adjacency in CSR form: two counting passes, no per-node vector.
    std::vector<std::uint32_t> offsets(item_count + 1, 0);
    for (const MatchPair& pair : pairs) {
        ++offsets[pair.a + 1];
        ++offsets[pair.b + 1];
    }
    std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());

    std::vector<std::uint32_t> neighbours(offsets.back());
    {
        std::vector<std::uint32_t> cursor(offsets.begin(), offsets.end() - 1);
        for (const MatchPair& pair : pairs) {
            neighbours[cursor[pair.a]++] = pair.b;
            neighbours[cursor[pair.b]++] = pair.a;
        }
    }

    std::vector<bool> assigned(item_count, false);
    std::vector<std::vector<std::uint32_t>> result;

    // Seeds are visited in index order, which makes the output stable across runs.
    for (std::uint32_t seed = 0; seed < item_count; ++seed) {
        if (assigned[seed] || offsets[seed] == offsets[seed + 1]) {
            continue;
        }

        std::vector<std::uint32_t> members{seed};
        assigned[seed] = true;

        // Only direct neighbours of the seed are admitted. This is what prevents
        // chaining: membership is always measured against one fixed reference.
        for (std::uint32_t slot = offsets[seed]; slot < offsets[seed + 1]; ++slot) {
            const std::uint32_t neighbour = neighbours[slot];
            if (!assigned[neighbour]) {
                assigned[neighbour] = true;
                members.push_back(neighbour);
            }
        }

        if (members.size() < 2) {
            continue;
        }
        std::sort(members.begin(), members.end());
        result.push_back(std::move(members));
    }

    return result;
}

std::uint32_t choose_keeper(std::span<const std::uint32_t> members,
                            const std::vector<KeeperInfo>& info, KeeperPolicy policy) {
    if (members.empty()) {
        return 0;
    }

    const auto better = [&](std::uint32_t candidate, std::uint32_t current) {
        const KeeperInfo& a = info[candidate];
        const KeeperInfo& b = info[current];

        switch (policy) {
            case KeeperPolicy::HighestResolution:
                if (a.pixels != b.pixels) {
                    return a.pixels > b.pixels;
                }
                break;
            case KeeperPolicy::LargestFile:
                if (a.size != b.size) {
                    return a.size > b.size;
                }
                break;
            case KeeperPolicy::OldestModified:
                if (a.mtime_ns != b.mtime_ns) {
                    return a.mtime_ns < b.mtime_ns;
                }
                break;
            case KeeperPolicy::NewestModified:
                if (a.mtime_ns != b.mtime_ns) {
                    return a.mtime_ns > b.mtime_ns;
                }
                break;
            case KeeperPolicy::ShortestPath:
                if (a.path_length != b.path_length) {
                    return a.path_length < b.path_length;
                }
                break;
        }

        // Shared tie-breakers, applied in a fixed order so the choice never
        // depends on enumeration order.
        if (a.size != b.size) {
            return a.size > b.size;
        }
        if (a.path_length != b.path_length) {
            return a.path_length < b.path_length;
        }
        return candidate < current;
    };

    std::uint32_t keeper = members.front();
    for (const std::uint32_t member : members.subspan(1)) {
        if (better(member, keeper)) {
            keeper = member;
        }
    }
    return keeper;
}

} // namespace ghidraengine
