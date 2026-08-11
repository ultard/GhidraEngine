#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ghidraengine/error.hpp"
#include "decode/thumbnail.hpp"

namespace ghidraengine {

// Dispatches on the leading bytes.
Result<Thumbnail> decode_image(std::span<const std::uint8_t> data);

// Straight out of the DCT domain, at the smallest scale still yielding
// kThumbSize pixels per axis.
Result<Thumbnail> decode_jpeg(std::span<const std::uint8_t> data);

// Everything else: PNG, WebP, AVIF, HEIF, TIFF, BMP, GIF, camera raw.
Result<Thumbnail> decode_with_ffmpeg(std::span<const std::uint8_t> data);

// Reads EXIF orientation (1-8) from a JPEG's APP1 segment. Returns 1 when absent.
std::uint16_t parse_exif_orientation(std::span<const std::uint8_t> app1);

} // namespace ghidraengine
