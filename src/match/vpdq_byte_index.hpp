#ifndef GHIDRAENGINE_VPDQ_BYTE_INDEX_HPP
#define GHIDRAENGINE_VPDQ_BYTE_INDEX_HPP

#include <GhidraEngine/hash/pdq.hpp>

#include "../cpu/kernels.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace GhidraEngine::detail {

class VpdqByteIndex {
public:
    explicit VpdqByteIndex(std::span<const PdqHash> hashes, std::uint16_t threshold = 31)
        : offsets_(std::size_t{32} * 257, 0), threshold_(threshold) {
        assert(hashes.size() <= 65536);
        assert(threshold < 256);

        for (const auto &hash : hashes) {
            for (std::size_t slot = 0; slot < 32; ++slot) {
                ++offsets_[slot * 257 + byte(hash, slot) + 1];
            }
        }

        for (std::size_t slot = 0; slot < 32; ++slot) {
            for (std::size_t value = 1; value <= 256; ++value) {
                offsets_[slot * 257 + value] += offsets_[slot * 257 + value - 1];
            }
        }
    }

    void build_positions(std::span<const PdqHash> hashes) {
        assert(hashes.size() <= 65536 && offsets_[256] == hashes.size());
        positions_.resize(32 * hashes.size());
        auto cursors = offsets_;

        for (std::size_t position = 0; position < hashes.size(); ++position) {
            for (std::size_t slot = 0; slot < 32; ++slot) {
                positions_
                    [slot * hashes.size() + cursors[slot * 257 + byte(hashes[position], slot)]++] =
                        static_cast<std::uint32_t>(position);
            }
        }
    }

    [[nodiscard]] std::size_t posting_visits(const PdqHash &query) const noexcept {
        std::size_t visits = 0;

        for (std::size_t slot = 0; slot < 32; ++slot) {
            for (std::size_t i = 0; i < mask_count(slot); ++i) {
                const auto bucket = slot * 257 + (byte(query, slot) ^ masks_[i]);
                visits += offsets_[bucket + 1] - offsets_[bucket];
            }
        }

        return visits;
    }

    [[nodiscard]] bool contains_match(
        const PdqHash &query,
        std::span<const PdqHash> hashes,
        std::uint16_t threshold
    ) const {
        assert(threshold == threshold_ && hashes.size() == positions_.size() / 32);
        const auto bounded = cpu_kernels().bounded;

        for (std::size_t slot = 0; slot < 32; ++slot) {
            const auto base = slot * hashes.size();

            for (std::size_t mask = 0; mask < mask_count(slot); ++mask) {
                const auto bucket = slot * 257 + (byte(query, slot) ^ masks_[mask]);

                for (auto i = offsets_[bucket]; i < offsets_[bucket + 1]; ++i) {
                    if (bounded(query, hashes[positions_[base + i]], threshold) <= threshold) {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    [[nodiscard]] std::size_t bucket_lookups() const noexcept {
        std::size_t count = 0;

        for (std::size_t slot = 0; slot < 32; ++slot) {
            count += mask_count(slot);
        }

        return count;
    }

private:
    inline static constexpr auto masks_ = [] {
        std::array<unsigned, 256> result{};
        std::size_t n = 0;

        for (int bits = 0; bits <= 8; ++bits) {
            for (unsigned mask = 0; mask < 256; ++mask) {
                if (std::popcount(mask) == bits) {
                    result[n++] = mask;
                }
            }
        }

        return result;
    }();

    std::size_t mask_count(std::size_t slot) const noexcept {
        constexpr std::array<std::size_t, 10> balls{0, 1, 9, 37, 93, 163, 219, 247, 255, 256};

        // Mixed radii: any omitted hash would need at least threshold + 1 changed bits.
        return balls[threshold_ / 32 + (slot <= threshold_ % 32 ? 1 : 0)];
    }

    static std::size_t byte(const PdqHash &hash, std::size_t slot) noexcept {
        return static_cast<std::size_t>((hash.words[slot / 8] >> ((slot % 8) * 8)) & 255U);
    }

    std::vector<std::size_t> offsets_;
    std::vector<std::uint32_t> positions_;
    std::uint16_t threshold_;
};

struct VpdqCoverageCount {
    std::size_t matches{};
    std::size_t sampled_comparisons{};
    bool used_index{};
};

inline VpdqCoverageCount vpdq_coverage_chunk(
    std::span<const PdqHash> from,
    std::span<const PdqHash> to,
    std::uint16_t threshold,
    std::span<std::uint8_t> matched = {}
) {
    const auto count_matches = [&](std::span<const PdqHash> hashes, const auto &contains) {
        return static_cast<std::size_t>(std::ranges::count_if(hashes, [&](const auto &hash) {
            const auto position = static_cast<std::size_t>(&hash - from.data());

            if (!matched.empty() && matched[position]) {
                return false;
            }

            const bool hit = contains(hash);

            if (!matched.empty() && hit) {
                matched[position] = 1;
            }

            return hit;
        }));
    };

    const auto contains = cpu_kernels().contains;
    const auto linear = [&](std::span<const PdqHash> hashes) {
        return count_matches(hashes, [&](const auto &hash) {
            return contains(hash, to, threshold);
        });
    };

    if (threshold == 256 || from.size() < 512 || to.size() < 512) {
        return {linear(from), 0, false};
    }

    // ponytail: 32-point cost sample; tune only if profiling shows poor algorithm selection.
    constexpr std::size_t probes = 32;
    std::array<bool, probes> sampled{};
    std::size_t comparisons = 0;
    const auto bounded = cpu_kernels().bounded;

    for (std::size_t n = 0; n < probes; ++n) {
        const auto position = n * (from.size() / probes);

        if (!matched.empty() && matched[position]) {
            continue;
        }

        const auto &query = from[position];
        sampled[n] = std::ranges::any_of(to, [&](const auto &other) {
            ++comparisons;

            return bounded(query, other, threshold) <= threshold;
        });
    }

    const auto build_cost = 64 * to.size() / from.size() + 32;
    const auto count_remaining = [&](const auto &count) {
        std::size_t matches = 0;
        std::size_t begin = 0;

        for (std::size_t n = 0; n < probes; ++n) {
            const auto position = n * (from.size() / probes);
            matches += count(from.subspan(begin, position - begin));
            matches += sampled[n] ? 1U : 0U;

            if (!matched.empty() && sampled[n]) {
                matched[position] = 1;
            }

            begin = position + 1;
        }

        return matches + count(from.subspan(begin));
    };

    if (comparisons / probes <= build_cost) {
        return {count_remaining(linear), comparisons, false};
    }

    VpdqByteIndex index(to, threshold);
    std::size_t visits = 0;

    for (std::size_t n = 0; n < probes; ++n) {
        visits += index.posting_visits(from[n * (from.size() / probes)]);
    }

    if (visits / probes + build_cost + index.bucket_lookups() >= comparisons / probes) {
        return {count_remaining(linear), comparisons, false};
    }

    index.build_positions(to);
    const auto matches = count_remaining([&](std::span<const PdqHash> hashes) {
        return count_matches(hashes, [&](const auto &hash) {
            return index.contains_match(hash, to, threshold);
        });
    });

    return {matches, comparisons, true};
}

inline VpdqCoverageCount vpdq_coverage_count(
    std::span<const PdqHash> from,
    std::span<const PdqHash> to,
    std::uint16_t threshold
) {
    constexpr std::size_t chunk_size = 65536; // At most 8 MiB of index postings.

    if (to.size() <= chunk_size) {
        return vpdq_coverage_chunk(from, to, threshold);
    }

    std::vector<std::uint8_t> matched(from.size(), 0);
    VpdqCoverageCount result;

    while (!to.empty() && result.matches < from.size()) {
        const auto size = std::min(chunk_size, to.size());
        const auto chunk = vpdq_coverage_chunk(from, to.first(size), threshold, matched);
        result.matches += chunk.matches;
        result.sampled_comparisons += chunk.sampled_comparisons;
        result.used_index |= chunk.used_index;
        to = to.subspan(size);
    }

    return result;
}

}

#endif
