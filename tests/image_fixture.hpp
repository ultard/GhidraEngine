// Synthetic images encoded as real JPEGs and pushed through the real decoder, so
// the accuracy tests cover what ships: DCT-domain scaling, resampling and
// thresholding together. Generated rather than checked in, so a test can ask for
// any size or quality it needs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ghidraengine::test {

struct Image {
    std::vector<std::uint8_t> rgb; // interleaved, 3 bytes per pixel
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Gradients plus seed-placed discs and bars. Structure matters: noise would give
// a perceptual hash nothing to latch onto and make every test vacuous.
Image make_image(std::uint32_t width, std::uint32_t height, std::uint32_t seed);

// Box-averaged rescale, used to test resolution invariance.
Image resize(const Image& source, std::uint32_t width, std::uint32_t height);

// Adds `delta` to every channel with saturation.
Image adjust_brightness(const Image& source, int delta);

Image rotate_90(const Image& source);
Image mirror_horizontal(const Image& source);

// Removes `percent` of each edge, then scales back to the original dimensions.
Image crop(const Image& source, double percent);

// Encodes to JPEG in memory at the given quality (1-100).
std::vector<std::uint8_t> encode_jpeg(const Image& image, int quality);

// Tests needing a non-JPEG file write raw BMP, which the FFmpeg fallback decodes.
std::vector<std::uint8_t> encode_bmp(const Image& image);

// Writes bytes to a file, creating parent directories.
void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);

// RAII temporary directory, removed on destruction.
class TempDir {
public:
    TempDir();
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace ghidraengine::test
