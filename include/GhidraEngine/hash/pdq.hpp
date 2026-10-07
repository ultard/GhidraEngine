#ifndef GHIDRAENGINE_HASH_PDQ_HPP
#define GHIDRAENGINE_HASH_PDQ_HPP

#include <GhidraEngine/core/fingerprints.hpp>
#include <GhidraEngine/export.hpp>

#include <bit>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace GhidraEngine {

struct PdqFingerprint {
    PdqHash hash;
    PdqQuality quality;
    bool operator==(const PdqFingerprint &) const = default;
};

[[nodiscard]] GHIDRAENGINE_EXPORT PdqFingerprint
compute_pdq(std::span<const float> luma, std::size_t width, std::size_t height);

namespace detail {

GHIDRAENGINE_EXPORT PdqDistance pdq_distance_runtime(const PdqHash &a, const PdqHash &b) noexcept;

}

[[nodiscard]] constexpr PdqDistance pdq_distance(const PdqHash &a, const PdqHash &b) noexcept {
    if (!std::is_constant_evaluated()) {
        return detail::pdq_distance_runtime(a, b);
    }

    int distance = 0;

    for (std::size_t i = 0; i < a.words.size(); ++i) {
        distance += std::popcount(a.words[i] ^ b.words[i]);
    }

    return PdqDistance{distance};
}

[[nodiscard]] GHIDRAENGINE_EXPORT std::string encode_pdq(const PdqHash &hash);

[[nodiscard]] GHIDRAENGINE_EXPORT PdqHash decode_pdq(std::string_view text);

}

#endif
