#ifndef GHIDRAENGINE_CORE_FINGERPRINTS_HPP
#define GHIDRAENGINE_CORE_FINGERPRINTS_HPP

#include <GhidraEngine/core/ids.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <stdexcept>

namespace GhidraEngine {

struct Blake3Digest {
    std::array<std::uint8_t, 32> bytes{};
    auto operator<=>(const Blake3Digest &) const = default;
};

struct PdqHash {
    std::array<std::uint64_t, 4> words{};
    auto operator<=>(const PdqHash &) const = default;
};

class PdqQuality {
public:
    constexpr PdqQuality() noexcept = default;

    explicit constexpr PdqQuality(std::int64_t value) {
        if (value < 0 || value > 100) {
            throw std::invalid_argument("PDQ quality must be in [0, 100]");
        }

        value_ = static_cast<std::uint8_t>(value);
    }

    [[nodiscard]] constexpr std::uint8_t value() const noexcept {
        return value_;
    }

    auto operator<=>(const PdqQuality &) const = default;

private:
    std::uint8_t value_{};
};

class PdqDistance {
public:
    constexpr PdqDistance() noexcept = default;

    explicit constexpr PdqDistance(const std::int64_t value) {
        if (value < 0 || value > 256) {
            throw std::invalid_argument("PDQ distance must be in [0, 256]");
        }

        value_ = static_cast<std::uint16_t>(value);
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    auto operator<=>(const PdqDistance &) const = default;

private:
    std::uint16_t value_{};
};

struct PdqHit {
    FingerprintId id;
    PdqDistance distance;
    auto operator<=>(const PdqHit &) const = default;
};

}

#endif
