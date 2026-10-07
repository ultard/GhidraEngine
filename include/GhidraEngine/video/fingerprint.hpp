#ifndef GHIDRAENGINE_VIDEO_FINGERPRINT_HPP
#define GHIDRAENGINE_VIDEO_FINGERPRINT_HPP

#include <GhidraEngine/video/decode.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>

namespace GhidraEngine {

struct VpdqGenerationOptions {
    VideoDecodeOptions decode{.sample_interval = Timestamp{1'000'000}};

    std::optional<PdqDistance> prune_distance;
    std::size_t max_frames = 1'000'000;
};

[[nodiscard]] GHIDRAENGINE_EXPORT VpdqSignature fingerprint_video(
    const std::filesystem::path &path,
    const VpdqGenerationOptions &options = {},
    const std::stop_token &stop = {}
);

[[nodiscard]] GHIDRAENGINE_EXPORT VpdqSignature
prune_vpdq(const VpdqSignature &signature, PdqDistance max_distance);

}
#endif
