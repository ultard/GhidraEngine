// Multi-Index Hashing: Hamming-radius search over 64-bit codes without the O(n^2)
// brute force (5*10^11 popcounts at a million images).
//
// Pigeonhole: split each code into k = 4 bands of 16 bits. Two codes within total
// distance d must agree in some band to within floor(d/k), so probing each band at
// that small radius surfaces every true match; an exact popcount removes the rest.
//
// A 16-bit band needs no hash function — the value indexes a 65536-entry array
// directly — and buckets are stored CSR, so each band is two flat arrays.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ghidraengine {

inline constexpr std::size_t kMihBands = 4;
inline constexpr std::size_t kMihBandBits = 16;
inline constexpr std::size_t kMihBandValues = 1U << kMihBandBits;

class MihIndex {
public:
    // Builds the index over `codes`. Item ids are indices into that span.
    void build(std::span<const std::uint64_t> codes);

    // Every item within `max_distance` of `code`, deduplicated and exact. `out` is
    // cleared first; `scratch` carries dedup state and must be one per thread.
    void query(std::uint64_t code, std::uint32_t max_distance, std::vector<std::uint32_t>& out,
               std::vector<std::uint32_t>& scratch, std::uint32_t& epoch) const;

    [[nodiscard]] std::size_t size() const noexcept { return codes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return codes_.empty(); }

    // Per-band search radius implied by a total distance budget.
    static constexpr std::uint32_t band_radius(std::uint32_t max_distance) noexcept {
        return max_distance / static_cast<std::uint32_t>(kMihBands);
    }

private:
    struct Band {
        // offsets has kMihBandValues + 1 entries; items[offsets[v] .. offsets[v+1])
        // lists every item whose band value is v.
        std::vector<std::uint32_t> offsets;
        std::vector<std::uint32_t> items;
    };

    static constexpr std::uint16_t band_of(std::uint64_t code, std::size_t band) noexcept {
        return static_cast<std::uint16_t>((code >> (band * kMihBandBits)) & 0xFFFFULL);
    }

    std::array<Band, kMihBands> bands_;
    std::vector<std::uint64_t> codes_;
};

// All 16-bit masks with popcount <= radius, generated once and cached. At the
// default threshold the radius is 2: 1 + 16 + 120 = 137 probes per band.
std::span<const std::uint16_t> hamming_probe_masks(std::uint32_t radius);

} // namespace ghidraengine
