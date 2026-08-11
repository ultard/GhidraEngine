#include "decode/resample.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace ghidraengine {
namespace {

std::vector<std::size_t> band_edges(std::size_t total, std::size_t parts) {
    std::vector<std::size_t> edges(parts + 1);
    for (std::size_t i = 0; i <= parts; ++i) {
        edges[i] = i * total / parts;
    }

    for (std::size_t i = 0; i < parts; ++i) {
        if (edges[i + 1] <= edges[i]) {
            edges[i + 1] = std::min(edges[i] + 1, total);
        }
    }
    return edges;
}

template <typename Transform>
void transform_plane(std::uint8_t* data, std::size_t size, Transform&& map) {
    std::vector<std::uint8_t> copy(data, data + size * size);
    for (std::size_t y = 0; y < size; ++y) {
        for (std::size_t x = 0; x < size; ++x) {
            const auto [sx, sy] = map(x, y, size);
            data[y * size + x] = copy[sy * size + sx];
        }
    }
}

}

void box_resample_channel(const std::uint8_t* source, std::size_t source_width,
                          std::size_t source_height, std::size_t source_stride,
                          std::size_t channels, std::size_t channel_index,
                          std::uint8_t* destination, std::size_t dst_width,
                          std::size_t dst_height) {
    if (source_width == 0 || source_height == 0 || dst_width == 0 || dst_height == 0) {
        return;
    }

    const std::vector<std::size_t> rows = band_edges(source_height, dst_height);
    const std::vector<std::size_t> cols = band_edges(source_width, dst_width);

    std::vector<std::uint32_t> accumulator(dst_width);

    for (std::size_t out_y = 0; out_y < dst_height; ++out_y) {
        std::fill(accumulator.begin(), accumulator.end(), 0U);

        const std::size_t y0 = std::min(rows[out_y], source_height - 1);
        const std::size_t y1 = std::max(y0 + 1, rows[out_y + 1]);

        for (std::size_t y = y0; y < y1 && y < source_height; ++y) {
            const std::uint8_t* row = source + y * source_stride + channel_index;
            for (std::size_t out_x = 0; out_x < dst_width; ++out_x) {
                const std::size_t x0 = std::min(cols[out_x], source_width - 1);
                const std::size_t x1 = std::max(x0 + 1, cols[out_x + 1]);
                std::uint32_t sum = 0;
                for (std::size_t x = x0; x < x1 && x < source_width; ++x) {
                    sum += row[x * channels];
                }
                accumulator[out_x] += sum / static_cast<std::uint32_t>(
                                          std::min(x1, source_width) - x0);
            }
        }

        const std::uint32_t row_count =
            static_cast<std::uint32_t>(std::min(y1, source_height) - y0);
        for (std::size_t out_x = 0; out_x < dst_width; ++out_x) {
            destination[out_y * dst_width + out_x] =
                static_cast<std::uint8_t>((accumulator[out_x] + row_count / 2) / row_count);
        }
    }
}

void apply_exif_orientation(Thumbnail& thumb, std::uint16_t orientation) {
    if (orientation <= 1 || orientation > 8) {
        return;
    }

    const auto map = [orientation](std::size_t x, std::size_t y, std::size_t size)
        -> std::pair<std::size_t, std::size_t> {
        const std::size_t last = size - 1;
        switch (orientation) {
            case 2: return {last - x, y};
            case 3: return {last - x, last - y};
            case 4: return {x, last - y};
            case 5: return {y, x};
            case 6: return {y, last - x};
            case 7: return {last - y, last - x};
            case 8: return {last - y, x};
            default: return {x, y};
        }
    };

    transform_plane(thumb.gray.data(), kThumbSize, map);
    if (thumb.has_color) {
        transform_plane(thumb.cb.data(), kChromaSize, map);
        transform_plane(thumb.cr.data(), kChromaSize, map);
    }

    if (orientation_transposes(orientation)) {
        std::swap(thumb.source_width, thumb.source_height);
    }
}

}
