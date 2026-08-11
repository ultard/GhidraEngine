#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ghidraengine::test {

struct Image {
    std::vector<std::uint8_t> rgb;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

Image make_image(std::uint32_t width, std::uint32_t height, std::uint32_t seed);

Image resize(const Image& source, std::uint32_t width, std::uint32_t height);

Image adjust_brightness(const Image& source, int delta);

Image rotate_90(const Image& source);
Image mirror_horizontal(const Image& source);

Image crop(const Image& source, double percent);

std::vector<std::uint8_t> encode_jpeg(const Image& image, int quality);

std::vector<std::uint8_t> encode_bmp(const Image& image);

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);

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

}
