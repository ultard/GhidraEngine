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
    void build(std::span<const std::uint64_t> codes);

    // `scratch` and `epoch` carry dedup state: one pair per thread.
    void query(std::uint64_t code, std::uint32_t max_distance, std::vector<std::uint32_t>& out,
               std::vector<std::uint32_t>& scratch, std::uint32_t& epoch) const;

    [[nodiscard]] std::size_t size() const noexcept { return codes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return codes_.empty(); }

    static constexpr std::uint32_t band_radius(std::uint32_t max_distance) noexcept {
        return max_distance / static_cast<std::uint32_t>(kMihBands);
    }

private:
    struct Band {
        std::vector<std::uint32_t> offsets;
        std::vector<std::uint32_t> items;
    };

    static constexpr std::uint16_t band_of(std::uint64_t code, std::size_t band) noexcept {
        return static_cast<std::uint16_t>((code >> (band * kMihBandBits)) & 0xFFFFULL);
    }

    std::array<Band, kMihBands> bands_;
    std::vector<std::uint64_t> codes_;
};

std::span<const std::uint16_t> hamming_probe_masks(std::uint32_t radius);

}
