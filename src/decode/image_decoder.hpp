#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ghidraengine/error.hpp"
#include "decode/thumbnail.hpp"

namespace ghidraengine {

Result<Thumbnail> decode_image(std::span<const std::uint8_t> data);

Result<Thumbnail> decode_jpeg(std::span<const std::uint8_t> data);

Result<Thumbnail> decode_with_ffmpeg(std::span<const std::uint8_t> data);

std::uint16_t parse_exif_orientation(std::span<const std::uint8_t> app1);

}
