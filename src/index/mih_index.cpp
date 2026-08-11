#include "index/mih_index.hpp"

#include <algorithm>
#include <bit>
#include <mutex>
#include <numeric>

namespace ghidraengine {
namespace {

constexpr std::uint32_t kMaxProbeRadius = 4;

std::vector<std::uint16_t> build_masks(std::uint32_t radius) {
    std::vector<std::uint16_t> masks;
    for (std::uint32_t value = 0; value < kMihBandValues; ++value) {
        if (static_cast<std::uint32_t>(std::popcount(value)) <= radius) {
            masks.push_back(static_cast<std::uint16_t>(value));
        }
    }
    std::stable_sort(masks.begin(), masks.end(),
                     [](std::uint16_t a, std::uint16_t b) {
                         return std::popcount(a) < std::popcount(b);
                     });
    return masks;
}

}

std::span<const std::uint16_t> hamming_probe_masks(std::uint32_t radius) {
    static std::once_flag flag;
    static std::array<std::vector<std::uint16_t>, kMaxProbeRadius + 1> cache;
    std::call_once(flag, [] {
        for (std::uint32_t r = 0; r <= kMaxProbeRadius; ++r) {
            cache[r] = build_masks(r);
        }
    });

    return cache[std::min(radius, kMaxProbeRadius)];
}

void MihIndex::build(std::span<const std::uint64_t> codes) {
    codes_.assign(codes.begin(), codes.end());
    const auto count = static_cast<std::uint32_t>(codes_.size());

    for (std::size_t band = 0; band < kMihBands; ++band) {
        Band& table = bands_[band];
        table.offsets.assign(kMihBandValues + 1, 0);
        table.items.resize(count);

        for (std::uint32_t i = 0; i < count; ++i) {
            ++table.offsets[band_of(codes_[i], band) + 1];
        }
        std::partial_sum(table.offsets.begin(), table.offsets.end(), table.offsets.begin());

        std::vector<std::uint32_t> cursor(table.offsets.begin(), table.offsets.end() - 1);
        for (std::uint32_t i = 0; i < count; ++i) {
            table.items[cursor[band_of(codes_[i], band)]++] = i;
        }
    }
}

void MihIndex::query(std::uint64_t code, std::uint32_t max_distance,
                     std::vector<std::uint32_t>& out, std::vector<std::uint32_t>& scratch,
                     std::uint32_t& epoch) const {
    out.clear();
    if (codes_.empty()) {
        return;
    }

    const std::uint32_t radius = band_radius(max_distance);

    if (radius > kMaxProbeRadius) {
        for (std::uint32_t item = 0; item < codes_.size(); ++item) {
            if (static_cast<std::uint32_t>(std::popcount(code ^ codes_[item])) <= max_distance) {
                out.push_back(item);
            }
        }
        return;
    }

    if (scratch.size() != codes_.size()) {
        scratch.assign(codes_.size(), 0);
        epoch = 0;
    }
    if (++epoch == 0) {
        std::fill(scratch.begin(), scratch.end(), 0);
        epoch = 1;
    }

    const std::span<const std::uint16_t> masks = hamming_probe_masks(radius);

    for (std::size_t band = 0; band < kMihBands; ++band) {
        const Band& table = bands_[band];
        const std::uint16_t key = band_of(code, band);

        for (const std::uint16_t mask : masks) {
            const std::uint16_t probe = static_cast<std::uint16_t>(key ^ mask);
            const std::uint32_t begin = table.offsets[probe];
            const std::uint32_t end = table.offsets[probe + 1];

            for (std::uint32_t slot = begin; slot < end; ++slot) {
                const std::uint32_t item = table.items[slot];

                if (static_cast<std::uint32_t>(std::popcount(code ^ codes_[item])) >
                    max_distance) {
                    continue;
                }
                if (scratch[item] == epoch) {
                    continue;
                }
                scratch[item] = epoch;
                out.push_back(item);
            }
        }
    }
}

}
