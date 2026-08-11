#pragma once

#include <filesystem>

#include "ghidraengine/config.hpp"
#include "ghidraengine/error.hpp"
#include "ghidraengine/types.hpp"

namespace ghidraengine {

Result<VideoSignature> extract_video_signature(const std::filesystem::path& path,
                                               const VideoMatchConfig& config);

}
