#ifndef GHIDRAENGINE_IMAGE_DECODE_HPP
#define GHIDRAENGINE_IMAGE_DECODE_HPP

#include <GhidraEngine/export.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <vector>

namespace GhidraEngine {

struct HdrToneMapOptions {
    double exposure = 1.0;
    double peak_nits = 1000.0;
};

struct ImageDecodeOptions {
    std::size_t max_dimension = 32768;
    std::size_t max_pixels = 40'000'000;
    std::array<std::uint8_t, 3> alpha_background = {255, 255, 255};
    HdrToneMapOptions hdr;
};

struct DecodedImage {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<float> luma;
};

[[nodiscard]] GHIDRAENGINE_EXPORT DecodedImage decode_image(
    const std::filesystem::path &path,
    const ImageDecodeOptions &options = {},
    const std::stop_token &stop = {}
);

}

#endif
