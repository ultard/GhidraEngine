#ifndef GHIDRAENGINE_CORE_IDS_HPP
#define GHIDRAENGINE_CORE_IDS_HPP

#include <compare>
#include <cstdint>

namespace GhidraEngine {

struct MediaId {
    std::uint64_t value{};
    auto operator<=>(const MediaId &) const = default;
};

struct VideoId {
    std::uint64_t value{};
    auto operator<=>(const VideoId &) const = default;
};

struct FingerprintId {
    std::uint64_t value{};
    auto operator<=>(const FingerprintId &) const = default;
};

}

#endif
