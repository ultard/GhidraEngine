#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <jpeglib.h>

#include "decode/image_decoder.hpp"
#include "decode/resample.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611 4324)
#endif

namespace ghidraengine {
namespace {

struct JpegError {
    jpeg_error_mgr base;
    std::jmp_buf escape;
    char message[JMSG_LENGTH_MAX];
};

// POD only: longjmp past a frame holding a non-trivial destructor is UB.
// `pixels` is malloc'd and owned by the caller.
struct JpegRaw {
    unsigned char* pixels = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    unsigned channels = 0;
    unsigned full_width = 0;
    unsigned full_height = 0;
    int has_color = 0;
    std::uint16_t orientation = 1;
    char message[JMSG_LENGTH_MAX] = {};
};

// Must not return, and must not let an exception cross libjpeg's C frames.
void on_fatal_error(j_common_ptr info) {
    auto* error = reinterpret_cast<JpegError*>(info->err);
    (*info->err->format_message)(info, error->message);
    std::longjmp(error->escape, 1);
}

void on_message(j_common_ptr, int) {}

unsigned choose_scale(unsigned width, unsigned height) {
    const unsigned smallest = std::min(width, height);
    for (unsigned numerator = 1; numerator <= 8; ++numerator) {
        if (smallest * numerator / 8 >= kThumbSize) {
            return numerator;
        }
    }
    return 8;
}

}

std::uint16_t parse_exif_orientation(std::span<const std::uint8_t> app1) {
    constexpr std::size_t kPrefix = 6;
    if (app1.size() < kPrefix + 8 || std::memcmp(app1.data(), "Exif\0\0", kPrefix) != 0) {
        return 1;
    }

    const std::uint8_t* tiff = app1.data() + kPrefix;
    const std::size_t tiff_size = app1.size() - kPrefix;

    const bool little_endian = tiff[0] == 'I' && tiff[1] == 'I';
    const bool big_endian = tiff[0] == 'M' && tiff[1] == 'M';
    if (!little_endian && !big_endian) {
        return 1;
    }

    const auto read16 = [&](std::size_t offset) -> std::uint16_t {
        if (offset + 2 > tiff_size) {
            return 0;
        }
        return little_endian
                   ? static_cast<std::uint16_t>(tiff[offset] | (tiff[offset + 1] << 8))
                   : static_cast<std::uint16_t>((tiff[offset] << 8) | tiff[offset + 1]);
    };
    const auto read32 = [&](std::size_t offset) -> std::uint32_t {
        if (offset + 4 > tiff_size) {
            return 0;
        }
        return little_endian
                   ? (static_cast<std::uint32_t>(tiff[offset]) |
                      (static_cast<std::uint32_t>(tiff[offset + 1]) << 8) |
                      (static_cast<std::uint32_t>(tiff[offset + 2]) << 16) |
                      (static_cast<std::uint32_t>(tiff[offset + 3]) << 24))
                   : ((static_cast<std::uint32_t>(tiff[offset]) << 24) |
                      (static_cast<std::uint32_t>(tiff[offset + 1]) << 16) |
                      (static_cast<std::uint32_t>(tiff[offset + 2]) << 8) |
                      static_cast<std::uint32_t>(tiff[offset + 3]));
    };

    if (read16(2) != 42) {
        return 1;
    }

    const std::uint32_t ifd_offset = read32(4);
    if (ifd_offset + 2 > tiff_size) {
        return 1;
    }

    const std::uint16_t entry_count = read16(ifd_offset);
    for (std::uint16_t i = 0; i < entry_count; ++i) {
        const std::size_t entry = ifd_offset + 2 + static_cast<std::size_t>(i) * 12;
        if (entry + 12 > tiff_size) {
            break;
        }
        if (read16(entry) == 0x0112) {
            const std::uint16_t value = read16(entry + 8);
            return (value >= 1 && value <= 8) ? value : 1;
        }
    }
    return 1;
}

namespace {

bool decode_jpeg_raw(const std::uint8_t* data, std::size_t size, JpegRaw& raw) {
    jpeg_decompress_struct info;
    JpegError error;

    info.err = jpeg_std_error(&error.base);
    error.base.error_exit = &on_fatal_error;
    error.base.emit_message = &on_message;
    error.message[0] = '\0';

    if (setjmp(error.escape) != 0) {
        std::memcpy(raw.message, error.message, sizeof(raw.message));
        if (raw.pixels != nullptr) {
            std::free(raw.pixels);
            raw.pixels = nullptr;
        }
        jpeg_destroy_decompress(&info);
        return false;
    }

    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, data, static_cast<unsigned long>(size));

    jpeg_save_markers(&info, JPEG_APP0 + 1, 0xFFFF);

    if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
        std::snprintf(raw.message, sizeof(raw.message), "header not recognised");
        jpeg_destroy_decompress(&info);
        return false;
    }

    for (jpeg_saved_marker_ptr marker = info.marker_list; marker != nullptr;
         marker = marker->next) {
        if (marker->marker == JPEG_APP0 + 1) {
            raw.orientation = parse_exif_orientation(
                std::span<const std::uint8_t>(marker->data, marker->data_length));
            break;
        }
    }

    raw.full_width = info.image_width;
    raw.full_height = info.image_height;
    if (raw.full_width == 0 || raw.full_height == 0) {
        std::snprintf(raw.message, sizeof(raw.message), "image reports zero dimensions");
        jpeg_destroy_decompress(&info);
        return false;
    }

    raw.has_color = info.num_components >= 3 ? 1 : 0;
    info.scale_num = choose_scale(raw.full_width, raw.full_height);
    info.scale_denom = 8;
    info.out_color_space = raw.has_color != 0 ? JCS_YCbCr : JCS_GRAYSCALE;
    info.do_fancy_upsampling = FALSE;
    info.do_block_smoothing = FALSE;
    info.dct_method = JDCT_ISLOW;

    jpeg_start_decompress(&info);

    raw.width = info.output_width;
    raw.height = info.output_height;
    raw.channels = static_cast<unsigned>(info.output_components);
    if (raw.width == 0 || raw.height == 0 || raw.channels == 0) {
        std::snprintf(raw.message, sizeof(raw.message), "decoder produced an empty image");
        jpeg_abort_decompress(&info);
        jpeg_destroy_decompress(&info);
        return false;
    }

    const std::size_t stride = static_cast<std::size_t>(raw.width) * raw.channels;
    raw.pixels = static_cast<unsigned char*>(std::malloc(stride * raw.height));
    if (raw.pixels == nullptr) {
        std::snprintf(raw.message, sizeof(raw.message), "out of memory");
        jpeg_abort_decompress(&info);
        jpeg_destroy_decompress(&info);
        return false;
    }

    while (info.output_scanline < raw.height) {
        JSAMPROW row = raw.pixels + static_cast<std::size_t>(info.output_scanline) * stride;
        if (jpeg_read_scanlines(&info, &row, 1) != 1) {
            break;
        }
    }
    const unsigned decoded_rows = info.output_scanline;

    if (decoded_rows == raw.height) {
        jpeg_finish_decompress(&info);
    } else {
        jpeg_abort_decompress(&info);
    }
    jpeg_destroy_decompress(&info);

    raw.height = decoded_rows;
    if (decoded_rows == 0) {
        std::free(raw.pixels);
        raw.pixels = nullptr;
        std::snprintf(raw.message, sizeof(raw.message), "no decodable scanlines");
        return false;
    }

    return true;
}

}

Result<Thumbnail> decode_jpeg(std::span<const std::uint8_t> data) {
    if (data.size() < 4) {
        return Error{ErrorCode::CorruptFile, "JPEG too short"};
    }

    JpegRaw raw;
    if (!decode_jpeg_raw(data.data(), data.size(), raw)) {
        return Error{ErrorCode::DecodeFailed, std::string("libjpeg: ") + raw.message};
    }

    const std::size_t stride = static_cast<std::size_t>(raw.width) * raw.channels;

    Thumbnail thumb;
    thumb.source_width = raw.full_width;
    thumb.source_height = raw.full_height;
    thumb.has_color = raw.has_color != 0;

    box_resample_channel(raw.pixels, raw.width, raw.height, stride, raw.channels, 0,
                         thumb.gray.data(), kThumbSize, kThumbSize);
    if (thumb.has_color) {
        box_resample_channel(raw.pixels, raw.width, raw.height, stride, raw.channels, 1,
                             thumb.cb.data(), kChromaSize, kChromaSize);
        box_resample_channel(raw.pixels, raw.width, raw.height, stride, raw.channels, 2,
                             thumb.cr.data(), kChromaSize, kChromaSize);
    }

    std::free(raw.pixels);

    apply_exif_orientation(thumb, raw.orientation);
    return thumb;
}

}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
