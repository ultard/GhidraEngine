#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ghidraengine/ghidraengine.hpp"

namespace ghidraengine {
namespace {

// Greedy one-to-one: without it, a static shot's near-identical frames would all
// be satisfied by a single frame of `b` and inflate the score.
std::uint32_t count_matching_frames(const VideoSignature& a, const VideoSignature& b,
                                    std::uint32_t threshold) {
    std::uint32_t matched = 0;
    std::uint64_t claimed = 0; // bitmask over b's frames; kMaxVideoFrames <= 64

    for (std::uint32_t i = 0; i < a.frame_count; ++i) {
        std::uint32_t best_distance = threshold + 1;
        int best_index = -1;

        for (std::uint32_t j = 0; j < b.frame_count; ++j) {
            if ((claimed & (1ULL << j)) != 0) {
                continue;
            }
            const std::uint32_t distance =
                static_cast<std::uint32_t>(std::popcount(a.frames[i] ^ b.frames[j]));
            if (distance < best_distance) {
                best_distance = distance;
                best_index = static_cast<int>(j);
            }
        }

        if (best_index >= 0 && best_distance <= threshold) {
            claimed |= 1ULL << best_index;
            ++matched;
        }
    }
    return matched;
}

// Shift of `a` within `b` maximising consecutive agreement. Valid because both
// signatures are sampled uniformly and so share a cadence.
double best_ordered_overlap(const VideoSignature& a, const VideoSignature& b,
                            std::uint32_t threshold) {
    if (a.frame_count == 0 || b.frame_count == 0) {
        return 0.0;
    }

    const auto& shorter = a.frame_count <= b.frame_count ? a : b;
    const auto& longer = a.frame_count <= b.frame_count ? b : a;

    double best = 0.0;
    const int max_shift = static_cast<int>(longer.frame_count) -
                          static_cast<int>(shorter.frame_count);

    for (int shift = 0; shift <= max_shift; ++shift) {
        std::uint32_t matched = 0;
        for (std::uint32_t i = 0; i < shorter.frame_count; ++i) {
            const std::uint32_t distance = static_cast<std::uint32_t>(
                std::popcount(shorter.frames[i] ^ longer.frames[i + shift]));
            if (distance <= threshold) {
                ++matched;
            }
        }
        best = std::max(best, static_cast<double>(matched) /
                                  static_cast<double>(shorter.frame_count));
    }
    return best;
}

} // namespace

double video_similarity(const VideoSignature& a, const VideoSignature& b,
                        const VideoMatchConfig& config) noexcept {
    if (a.frame_count == 0 || b.frame_count == 0) {
        return 0.0;
    }

    // Cheap gate that keeps the quadratic frame matching off the critical path.
    if (!config.subclip_detection && a.duration_ms > 0 && b.duration_ms > 0) {
        const double longer = static_cast<double>(std::max(a.duration_ms, b.duration_ms));
        const double delta =
            static_cast<double>(std::llabs(a.duration_ms - b.duration_ms));
        if (delta / longer > config.duration_tolerance) {
            return 0.0;
        }
    }

    if (config.subclip_detection) {
        // A cut clip shares an ordered run; the unordered score is kept as a floor
        // for the equal-length case.
        const double ordered = best_ordered_overlap(a, b, config.frame_threshold);
        const std::uint32_t matched = count_matching_frames(a, b, config.frame_threshold);
        const double unordered = static_cast<double>(matched) /
                                 static_cast<double>(std::min(a.frame_count, b.frame_count));
        return std::max(ordered, unordered);
    }

    const std::uint32_t matched = count_matching_frames(a, b, config.frame_threshold);
    // By the smaller count, so a signature that lost frames is not penalised twice.
    return static_cast<double>(matched) /
           static_cast<double>(std::min(a.frame_count, b.frame_count));
}

} // namespace ghidraengine
