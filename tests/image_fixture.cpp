#include "image_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include <jpeglib.h>

namespace ghidraengine::test {
namespace {

std::uint8_t saturate(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

} // namespace

Image make_image(std::uint32_t width, std::uint32_t height, std::uint32_t seed) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3);

    std::mt19937 rng(seed);
    // Shapes are large on purpose: a DCT hash encodes low-frequency structure.
    struct Disc {
        double x;
        double y;
        double radius;
        int r;
        int g;
        int b;
    };
    std::vector<Disc> discs;
    std::uniform_real_distribution<double> position(0.15, 0.85);
    std::uniform_real_distribution<double> radius(0.08, 0.28);
    std::uniform_int_distribution<int> channel(0, 255);
    for (int i = 0; i < 6; ++i) {
        discs.push_back(Disc{position(rng), position(rng), radius(rng), channel(rng),
                             channel(rng), channel(rng)});
    }

    for (std::uint32_t y = 0; y < height; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(width);

            int r = static_cast<int>(220.0 * fx);
            int g = static_cast<int>(200.0 * fy);
            int b = static_cast<int>(180.0 * (1.0 - 0.5 * (fx + fy)));

            for (const Disc& disc : discs) {
                const double dx = fx - disc.x;
                const double dy = fy - disc.y;
                const double distance = std::sqrt(dx * dx + dy * dy);
                if (distance < disc.radius) {
                    // Soft edge, so downscaling gives a gradient not a staircase.
                    const double weight = 1.0 - (distance / disc.radius);
                    r = static_cast<int>(r * (1.0 - weight) + disc.r * weight);
                    g = static_cast<int>(g * (1.0 - weight) + disc.g * weight);
                    b = static_cast<int>(b * (1.0 - weight) + disc.b * weight);
                }
            }

            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3;
            image.rgb[offset + 0] = saturate(r);
            image.rgb[offset + 1] = saturate(g);
            image.rgb[offset + 2] = saturate(b);
        }
    }

    return image;
}

Image resize(const Image& source, std::uint32_t width, std::uint32_t height) {
    Image result;
    result.width = width;
    result.height = height;
    result.rgb.resize(static_cast<std::size_t>(width) * height * 3);

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t y0 = y * source.height / height;
        const std::uint32_t y1 = std::max(y0 + 1, (y + 1) * source.height / height);

        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t x0 = x * source.width / width;
            const std::uint32_t x1 = std::max(x0 + 1, (x + 1) * source.width / width);

            std::uint32_t sums[3] = {0, 0, 0};
            std::uint32_t count = 0;
            for (std::uint32_t sy = y0; sy < y1 && sy < source.height; ++sy) {
                for (std::uint32_t sx = x0; sx < x1 && sx < source.width; ++sx) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(sy) * source.width + sx) * 3;
                    sums[0] += source.rgb[offset + 0];
                    sums[1] += source.rgb[offset + 1];
                    sums[2] += source.rgb[offset + 2];
                    ++count;
                }
            }

            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3;
            for (int c = 0; c < 3; ++c) {
                result.rgb[offset + c] = static_cast<std::uint8_t>(sums[c] / std::max(1U, count));
            }
        }
    }
    return result;
}

Image adjust_brightness(const Image& source, int delta) {
    Image result = source;
    for (std::uint8_t& value : result.rgb) {
        value = saturate(static_cast<int>(value) + delta);
    }
    return result;
}

Image rotate_90(const Image& source) {
    Image result;
    result.width = source.height;
    result.height = source.width;
    result.rgb.resize(source.rgb.size());

    for (std::uint32_t y = 0; y < result.height; ++y) {
        for (std::uint32_t x = 0; x < result.width; ++x) {
            const std::uint32_t sx = y;
            const std::uint32_t sy = source.height - 1 - x;
            const std::size_t from = (static_cast<std::size_t>(sy) * source.width + sx) * 3;
            const std::size_t to = (static_cast<std::size_t>(y) * result.width + x) * 3;
            std::memcpy(result.rgb.data() + to, source.rgb.data() + from, 3);
        }
    }
    return result;
}

Image mirror_horizontal(const Image& source) {
    Image result = source;
    for (std::uint32_t y = 0; y < source.height; ++y) {
        for (std::uint32_t x = 0; x < source.width; ++x) {
            const std::size_t from =
                (static_cast<std::size_t>(y) * source.width + (source.width - 1 - x)) * 3;
            const std::size_t to = (static_cast<std::size_t>(y) * source.width + x) * 3;
            std::memcpy(result.rgb.data() + to, source.rgb.data() + from, 3);
        }
    }
    return result;
}

Image crop(const Image& source, double percent) {
    const auto margin_x = static_cast<std::uint32_t>(source.width * percent);
    const auto margin_y = static_cast<std::uint32_t>(source.height * percent);
    const std::uint32_t width = source.width - 2 * margin_x;
    const std::uint32_t height = source.height - 2 * margin_y;

    Image cropped;
    cropped.width = width;
    cropped.height = height;
    cropped.rgb.resize(static_cast<std::size_t>(width) * height * 3);

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t from =
            (static_cast<std::size_t>(y + margin_y) * source.width + margin_x) * 3;
        const std::size_t to = static_cast<std::size_t>(y) * width * 3;
        std::memcpy(cropped.rgb.data() + to, source.rgb.data() + from,
                    static_cast<std::size_t>(width) * 3);
    }

    return resize(cropped, source.width, source.height);
}

std::vector<std::uint8_t> encode_jpeg(const Image& image, int quality) {
    jpeg_compress_struct info{};
    jpeg_error_mgr error{};
    info.err = jpeg_std_error(&error);
    jpeg_create_compress(&info);

    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&info, &buffer, &size);

    info.image_width = image.width;
    info.image_height = image.height;
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, quality, TRUE);
    jpeg_start_compress(&info, TRUE);

    while (info.next_scanline < info.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(
            image.rgb.data() + static_cast<std::size_t>(info.next_scanline) * image.width * 3);
        jpeg_write_scanlines(&info, &row, 1);
    }

    jpeg_finish_compress(&info);
    std::vector<std::uint8_t> result(buffer, buffer + size);
    jpeg_destroy_compress(&info);
    free(buffer);
    return result;
}

std::vector<std::uint8_t> encode_bmp(const Image& image) {
    // 24-bit bottom-up BMP: rows are padded to a multiple of four bytes.
    const std::uint32_t row_bytes = image.width * 3;
    const std::uint32_t padding = (4 - (row_bytes % 4)) % 4;
    const std::uint32_t pixel_bytes = (row_bytes + padding) * image.height;
    const std::uint32_t offset = 14 + 40;
    const std::uint32_t total = offset + pixel_bytes;

    std::vector<std::uint8_t> bytes(total, 0);

    const auto put16 = [&](std::size_t at, std::uint16_t value) {
        bytes[at] = static_cast<std::uint8_t>(value & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        bytes[at] = static_cast<std::uint8_t>(value & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        bytes[at + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        bytes[at + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    };

    bytes[0] = 'B';
    bytes[1] = 'M';
    put32(2, total);
    put32(10, offset);
    put32(14, 40);
    put32(18, image.width);
    put32(22, image.height);
    put16(26, 1);
    put16(28, 24);
    put32(34, pixel_bytes);

    for (std::uint32_t y = 0; y < image.height; ++y) {
        const std::uint32_t source_row = image.height - 1 - y;
        std::size_t at = offset + static_cast<std::size_t>(y) * (row_bytes + padding);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t from =
                (static_cast<std::size_t>(source_row) * image.width + x) * 3;
            bytes[at++] = image.rgb[from + 2]; // BMP stores BGR
            bytes[at++] = image.rgb[from + 1];
            bytes[at++] = image.rgb[from + 0];
        }
    }

    return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::FILE* handle = nullptr;
#ifdef _WIN32
    _wfopen_s(&handle, path.native().c_str(), L"wb");
#else
    handle = std::fopen(path.c_str(), "wb");
#endif
    if (handle == nullptr) {
        return;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), handle);
    std::fclose(handle);
}

TempDir::TempDir() {
    static std::atomic<int> counter{0};
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count() + counter.fetch_add(1));
    path_ = std::filesystem::temp_directory_path() / ("ghidraengine_test_" + unique);
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

} // namespace ghidraengine::test
