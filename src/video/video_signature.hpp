#pragma once

#include <filesystem>

#include "ghidraengine/config.hpp"
#include "ghidraengine/error.hpp"
#include "ghidraengine/types.hpp"

namespace ghidraengine {

// Samples keyframes across the timeline and hashes each one. AVDISCARD_NONKEY
// drops non-keyframe packets before decoding, and the decoder is deliberately
// single-threaded: cost per file is seek plus one intra frame, so parallelism
// belongs across files, not within one.
Result<VideoSignature> extract_video_signature(const std::filesystem::path& path,
                                               const VideoMatchConfig& config);

} // namespace ghidraengine
