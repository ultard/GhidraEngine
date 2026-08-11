#include "ghidraengine/config.hpp"

#include <string>

namespace ghidraengine {

Result<void> ScanConfig::validate() const {
    if (!detect_exact && !detect_similar) {
        return Error{ErrorCode::InvalidArgument,
                     "both detect_exact and detect_similar are disabled: nothing to do"};
    }
    if (!scan_images && !scan_videos) {
        return Error{ErrorCode::InvalidArgument,
                     "both scan_images and scan_videos are disabled: nothing to do"};
    }
    if (max_file_size != 0 && max_file_size < min_file_size) {
        return Error{ErrorCode::InvalidArgument, "max_file_size is below min_file_size"};
    }
    if (image.phash_threshold > 64) {
        return Error{ErrorCode::InvalidArgument, "phash_threshold exceeds the 64-bit hash width"};
    }
    if (image.phash256_threshold > 256) {
        return Error{ErrorCode::InvalidArgument, "phash256_threshold exceeds the 256-bit hash width"};
    }
    if (image.dhash_threshold > 64) {
        return Error{ErrorCode::InvalidArgument, "dhash_threshold exceeds the 64-bit hash width"};
    }
    if (image.color_threshold > 255) {
        return Error{ErrorCode::InvalidArgument, "color_threshold exceeds the 0-255 range"};
    }
    if (image.min_dimension < 8) {
        return Error{ErrorCode::InvalidArgument,
                     "min_dimension below 8 leaves too little signal for a perceptual hash"};
    }
    if (video.frame_samples == 0) {
        return Error{ErrorCode::InvalidArgument, "video.frame_samples must be at least 1"};
    }
    if (video.frame_samples > kMaxVideoFrames) {
        return Error{ErrorCode::InvalidArgument,
                     "video.frame_samples exceeds kMaxVideoFrames (" +
                         std::to_string(kMaxVideoFrames) + ")"};
    }
    if (video.edge_skip_fraction < 0.0 || video.edge_skip_fraction >= 0.5) {
        return Error{ErrorCode::InvalidArgument,
                     "video.edge_skip_fraction must be in [0, 0.5)"};
    }
    if (video.duration_tolerance < 0.0 || video.duration_tolerance > 1.0) {
        return Error{ErrorCode::InvalidArgument, "video.duration_tolerance must be in [0, 1]"};
    }
    if (video.frame_threshold > 64) {
        return Error{ErrorCode::InvalidArgument, "video.frame_threshold exceeds the hash width"};
    }
    if (video.min_frame_match_ratio <= 0.0 || video.min_frame_match_ratio > 1.0) {
        return Error{ErrorCode::InvalidArgument,
                     "video.min_frame_match_ratio must be in (0, 1]"};
    }
    return {};
}

}
