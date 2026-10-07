#ifndef GHIDRAENGINE_CORE_MEDIA_HPP
#define GHIDRAENGINE_CORE_MEDIA_HPP

#include <GhidraEngine/core/fingerprints.hpp>

#include <chrono>
#include <cstdint>
#include <ratio>
#include <stdexcept>
#include <vector>

namespace GhidraEngine {

using Timestamp = std::chrono::duration<std::int64_t, std::micro>;

struct VpdqFrame {
    PdqHash hash;
    PdqQuality quality;
    Timestamp timestamp{};
    bool operator==(const VpdqFrame &) const = default;
};

struct VpdqSignature {
    std::vector<VpdqFrame> frames;
    bool operator==(const VpdqSignature &) const = default;
};

enum class TransformPolicy : std::uint8_t {
    OriginalOnly,
    Rotations,
    Dihedral
};

struct PdqMatchPolicy {
    PdqDistance max_distance;
    PdqQuality min_quality;
    bool operator==(const PdqMatchPolicy &) const = default;
};

class Coverage {
public:
    constexpr Coverage() noexcept = default;

    explicit constexpr Coverage(const double value) {
        if (!(value >= 0.0 && value <= 1.0)) {
            throw std::invalid_argument("Coverage must be a finite fraction in [0, 1]");
        }

        value_ = value;
    }

    [[nodiscard]] constexpr double value() const noexcept {
        return value_;
    }

    bool operator==(const Coverage &) const = default;

private:
    double value_{};
};

struct ImageMatch {
    MediaId query;
    MediaId candidate;
    PdqDistance distance;
    bool operator==(const ImageMatch &) const = default;
};

struct VideoMatch {
    VideoId query;
    VideoId candidate;
    Coverage query_coverage;
    Coverage candidate_coverage;
    bool operator==(const VideoMatch &) const = default;
};

}

#endif
