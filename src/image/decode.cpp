#include <GhidraEngine/image/decode.hpp>

#include "../cpu/kernels.hpp"
#include "../decode_options.hpp"
#include "../hdr.hpp"

#include <vips/vips.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace GhidraEngine {

namespace {

struct Unref {
    template <class T>
    void operator()(T *object) const noexcept {
        g_object_unref(object);
    }
};
template <class T>
using VipsPtr = std::unique_ptr<T, Unref>;

struct Free {
    void operator()(void *text) const noexcept {
        g_free(text);
    }
};

void check_cancelled(const std::stop_token &stop) {
    if (stop.stop_requested()) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_canceled),
            "Image decoding cancelled"
        );
    }
}

[[noreturn]] void fail(const std::filesystem::path &path, std::string_view stage) {
    const std::unique_ptr<char, Free> detail(vips_error_buffer_copy());
    throw std::filesystem::filesystem_error(
        std::string(stage) + ": " + (detail ? detail.get() : "libvips failure"),
        path,
        std::make_error_code(std::errc::io_error)
    );
}

[[noreturn]] void reject(const std::filesystem::path &path, std::string_view reason) {
    throw std::filesystem::filesystem_error(
        std::string(reason),
        path,
        std::make_error_code(std::errc::invalid_argument)
    );
}

void initialize() {
    static const bool initialized = [] {
        if (vips_init("GhidraEngine") != 0) {
            throw std::runtime_error(
                std::string("Cannot initialize libvips: ") + vips_error_buffer()
            );
        }

        vips_concurrency_set(1);
        vips_cache_set_max(0);

        return true;
    }();
    (void)initialized;
}

void validate_dimensions(
    VipsImage *image,
    const std::filesystem::path &path,
    const ImageDecodeOptions &options
) {
    const int width = vips_image_get_width(image);
    const int height = vips_image_get_height(image);

    if (width <= 0 || height <= 0) {
        reject(path, "Image dimensions exceed configured limits");
    }

    const auto columns = static_cast<std::size_t>(width);
    const auto rows = static_cast<std::size_t>(height);
    const bool dimension_limit = columns > options.max_dimension || rows > options.max_dimension;
    const bool pixel_limit = columns > options.max_pixels / rows;

    if (dimension_limit || pixel_limit) {
        reject(path, "Image dimensions exceed configured limits");
    }
}

int metadata_integer(
    VipsImage *image,
    const char *name,
    int fallback,
    const std::filesystem::path &path
) {
    if (vips_image_get_typeof(image, name) == 0) {
        return fallback;
    }

    int value = 0;

    if (vips_image_get_int(image, name, &value) != 0) {
        fail(path, "Invalid integer image metadata");
    }

    return value;
}

class Warnings {
public:
    Warnings() : handler_(g_log_set_handler("VIPS", G_LOG_LEVEL_WARNING, record, this)) {
    }

    Warnings(const Warnings &) = delete;

    Warnings &operator=(const Warnings &) = delete;

    ~Warnings() {
        g_log_remove_handler("VIPS", handler_);
    }

    void check(const std::filesystem::path &path) const {
        if (seen_.load()) {
            reject(path, "libvips warned about invalid metadata or damaged image data");
        }
    }

private:
    static void record(
        const gchar *domain,
        GLogLevelFlags level,
        const gchar *message,
        gpointer context
    ) noexcept {
        static_cast<Warnings *>(context)->seen_.store(true);
        g_log_default_handler(domain, level, message, nullptr);
    }

    std::atomic<bool> seen_{false};
    guint handler_;
};

VipsPtr<VipsImage> load(const std::filesystem::path &path, VipsAccess access) {
    const auto utf8 = path.u8string();
    const auto *filename = reinterpret_cast<const char *>(utf8.c_str());

    if (utf8.find(u8'\0') != std::u8string::npos ||
        !g_utf8_validate(filename, static_cast<gssize>(utf8.size()), nullptr)) {
        reject(path, "Image path must be valid UTF-8 without embedded NUL");
    }

    const VipsPtr<VipsSource> source(vips_source_new_from_file(filename));

    if (!source) {
        fail(path, "Cannot open image source");
    }

    VipsPtr<VipsImage> image(vips_image_new_from_source(
        source.get(),
        "",
        "access",
        access,
        "fail_on",
        VIPS_FAIL_ON_WARNING,
        nullptr
    ));

    if (!image) {
        fail(path, "Cannot load image header");
    }

    return image;
}

VipsPtr<VipsImage> convert_colors(VipsImage *input, const std::filesystem::path &path) {
    VipsImage *output = nullptr;

    if (vips_image_get_typeof(input, VIPS_META_ICC_NAME) != 0) {
        const void *profile = nullptr;
        std::size_t length = 0;

        if (vips_image_get_blob(input, VIPS_META_ICC_NAME, &profile, &length) != 0) {
            fail(path, "Cannot read ICC profile");
        }

        if (!vips_icc_is_compatible_profile(input, profile, length)) {
            reject(path, "Invalid or incompatible ICC profile");
        }

        if (vips_icc_transform(
                input,
                &output,
                "srgb",
                "embedded",
                TRUE,
                "depth",
                8,
                "intent",
                VIPS_INTENT_RELATIVE,
                nullptr
            ) != 0) {
            fail(path, "Cannot convert embedded ICC profile to sRGB");
        }
    } else if (vips_colourspace(input, &output, VIPS_INTERPRETATION_sRGB, nullptr) != 0) {
        fail(path, "Cannot convert image colors to sRGB");
    }

    VipsPtr<VipsImage> rgb(output);

    if (vips_image_get_bands(rgb.get()) != 3 ||
        vips_image_get_format(rgb.get()) != VIPS_FORMAT_UCHAR) {
        reject(path, "Color conversion did not produce RGB8");
    }

    return rgb;
}

VipsPtr<VipsImage> normalize(VipsImage *input, const std::filesystem::path &path) {
    VipsPtr<VipsImage> unpacked;

    if (vips_image_get_coding(input) == VIPS_CODING_RAD) {
        VipsImage *output = nullptr;

        if (vips_rad2float(input, &output, nullptr) != 0) {
            fail(path, "Cannot unpack Radiance pixels");
        }

        unpacked.reset(output);
        input = unpacked.get();
    }

    const auto format = vips_image_get_format(input);

    if ((format == VIPS_FORMAT_FLOAT || format == VIPS_FORMAT_DOUBLE) &&
        vips_image_get_coding(input) == VIPS_CODING_NONE) {
        const auto interpretation = vips_image_get_interpretation(input);
        const auto bands = vips_image_get_bands(input);

        if (bands < 1 || bands > 4 ||
            (interpretation != VIPS_INTERPRETATION_scRGB &&
             interpretation != VIPS_INTERPRETATION_RGB &&
             interpretation != VIPS_INTERPRETATION_B_W &&
             interpretation != VIPS_INTERPRETATION_MULTIBAND)) {
            reject(path, "HDR images require linear RGB or gray samples");
        }

        if (vips_image_get_typeof(input, VIPS_META_ICC_NAME) != 0) {
            reject(path, "Floating HDR images with ICC profiles are unsupported");
        }

        g_object_ref(input);

        return VipsPtr<VipsImage>(input);
    }

    if ((format != VIPS_FORMAT_UCHAR && format != VIPS_FORMAT_USHORT) ||
        vips_image_get_coding(input) != VIPS_CODING_NONE) {
        reject(path, "Only uncoded unsigned 8/16-bit image samples are supported");
    }

    VipsPtr<VipsImage> alpha;
    VipsPtr<VipsImage> colors;
    VipsImage *output = nullptr;

    if (vips_image_hasalpha(input)) {
        const int bands = vips_image_get_bands(input);

        if (vips_extract_band(input, &output, bands - 1, nullptr) != 0) {
            fail(path, "Cannot extract alpha");
        }

        alpha.reset(output);

        if (vips_extract_band(input, &output, 0, "n", bands - 1, nullptr) != 0) {
            fail(path, "Cannot extract color channels");
        }

        colors.reset(output);
        input = colors.get();
    }

    auto rgb = convert_colors(input, path);

    if (!alpha) {
        return rgb;
    }

    if (vips_bandjoin2(rgb.get(), alpha.get(), &output, nullptr) != 0) {
        fail(path, "Cannot join color and alpha channels");
    }

    return VipsPtr<VipsImage>(output);
}

template <class Channel>
void convert_hdr_row(
    const Channel *row,
    std::span<float> luma,
    unsigned bands,
    const ImageDecodeOptions &options,
    const std::filesystem::path &path,
    double white_squared
) {
    const bool gray = bands <= 2;

    for (std::size_t x = 0; x < luma.size(); ++x) {
        const auto *pixel = row + x * bands;
        const std::array<double, 3> linear{pixel[0], pixel[gray ? 0 : 1], pixel[gray ? 0 : 2]};
        std::array<unsigned, 3> rgb{};

        try {
            rgb = detail::tone_map(linear, options.hdr, white_squared);
        } catch (const std::invalid_argument &) {
            reject(path, "Non-finite HDR image sample");
        }

        if (bands == 2 || bands == 4) {
            const double alpha = pixel[bands - 1];

            if (!std::isfinite(alpha) || alpha < 0 || alpha > 1) {
                reject(path, "Invalid HDR alpha");
            }

            for (std::size_t c = 0; c < 3; ++c) {
                const auto blended = rgb[c] * alpha + options.alpha_background[c] * (1 - alpha);
                rgb[c] = static_cast<unsigned>(std::lround(blended));
            }
        }

        luma[x] = 0.299F * static_cast<float>(rgb[0]) + 0.587F * static_cast<float>(rgb[1]) +
                  0.114F * static_cast<float>(rgb[2]);
    }
}

struct NativePixels {
    std::size_t width;
    std::size_t height;
    unsigned bands;
    VipsBandFormat format;
    std::unique_ptr<std::uint8_t[]> data;
};

DecodedImage read_luma(
    const NativePixels &pixels,
    const std::filesystem::path &path,
    const ImageDecodeOptions &options,
    const std::stop_token &stop
) {
    const auto width = pixels.width;
    const auto height = pixels.height;
    DecodedImage result{width, height, std::vector<float>(width * height)};
    const auto bands = pixels.bands;
    const auto *data = pixels.data.get();
    const auto channel_size =
        pixels.format == VIPS_FORMAT_DOUBLE   ? sizeof(double) :
        pixels.format == VIPS_FORMAT_FLOAT    ? sizeof(float) :
        pixels.format == VIPS_FORMAT_USHORT   ? sizeof(std::uint16_t) : 1;
    const auto &kernels = detail::cpu_kernels();
    const auto white = options.hdr.peak_nits / 203.0;
    const auto white_squared = white * white;

    for (std::size_t y = 0; y < height; ++y) {
        check_cancelled(stop);

        const auto *row = data + y * width * bands * channel_size;
        const auto luma = std::span<float>(result.luma).subspan(y * width, width);

        if (pixels.format == VIPS_FORMAT_FLOAT) {
            convert_hdr_row(
                reinterpret_cast<const float *>(row),
                luma,
                bands,
                options,
                path,
                white_squared
            );
        } else if (pixels.format == VIPS_FORMAT_DOUBLE) {
            convert_hdr_row(
                reinterpret_cast<const double *>(row),
                luma,
                bands,
                options,
                path,
                white_squared
            );
        } else if (pixels.format == VIPS_FORMAT_USHORT) {
            kernels.luma16(
                reinterpret_cast<const std::uint16_t *>(row),
                luma,
                bands,
                options.alpha_background
            );
        } else {
            kernels.luma8(row, luma, bands, options.alpha_background);
        }
    }

    return result;
}

VipsPtr<VipsImage> prepare_pixels(
    const std::filesystem::path &path,
    const ImageDecodeOptions &options,
    const Warnings &warnings
) {
    auto image = load(path, VIPS_ACCESS_SEQUENTIAL);
    warnings.check(path);
    validate_dimensions(image.get(), path, options);

    if (metadata_integer(image.get(), "n-pages", 1, path) != 1) {
        reject(path, "Multi-page or animated images require an explicit future frame policy");
    }

    const int orientation = metadata_integer(image.get(), VIPS_META_ORIENTATION, 1, path);

    if (orientation < 1 || orientation > 8) {
        reject(path, "EXIF orientation must be in [1, 8]");
    }

    if (orientation > 2) {
        image = load(path, VIPS_ACCESS_RANDOM);
        validate_dimensions(image.get(), path, options);
    }

    VipsImage *output = nullptr;

    if (vips_autorot(image.get(), &output, nullptr) != 0) {
        fail(path, "Cannot apply EXIF orientation");
    }

    const VipsPtr<VipsImage> oriented(output);
    validate_dimensions(oriented.get(), path, options);
    auto pixels = normalize(oriented.get(), path);
    warnings.check(path);

    return pixels;
}

}

DecodedImage decode_image(
    const std::filesystem::path &path,
    const ImageDecodeOptions &options,
    const std::stop_token &stop
) {
    check_cancelled(stop);
    detail::check_decode_options(options);
    std::error_code error;
    const auto status = std::filesystem::status(path, error);

    if (error) {
        throw std::filesystem::filesystem_error("Cannot inspect image", path, error);
    }

    if (!std::filesystem::is_regular_file(status)) {
        reject(path, "Image input must be a regular file");
    }

    const auto native = [&] {
        // ponytail: libvips 8.16 has global errors/warnings; only native work needs this lock.
        static std::mutex decoder_mutex;
        const std::lock_guard lock(decoder_mutex);
        check_cancelled(stop);
        initialize();
        vips_error_clear();

        const Warnings warnings;
        const auto pixels = prepare_pixels(path, options, warnings);
        NativePixels result{
            static_cast<std::size_t>(vips_image_get_width(pixels.get())),
            static_cast<std::size_t>(vips_image_get_height(pixels.get())),
            static_cast<unsigned>(vips_image_get_bands(pixels.get())),
            vips_image_get_format(pixels.get()),
            {}
        };
        const auto row_bytes = VIPS_IMAGE_SIZEOF_LINE(pixels.get());
        result.data = std::make_unique_for_overwrite<std::uint8_t[]>(row_bytes * result.height);
        const VipsPtr<VipsRegion> region(vips_region_new(pixels.get()));

        if (!region) {
            fail(path, "Cannot create image region");
        }

        // Evaluate rows directly; write_to_memory starts a sink thread even for small images.
        for (std::size_t y = 0; y < result.height; ++y) {
            check_cancelled(stop);
            const VipsRect rect{0, static_cast<int>(y), static_cast<int>(result.width), 1};

            if (vips_region_prepare(region.get(), &rect) != 0) {
                fail(path, "Cannot decode image pixels");
            }

            std::memcpy(
                result.data.get() + y * row_bytes,
                VIPS_REGION_ADDR(region.get(), 0, static_cast<int>(y)),
                row_bytes
            );
        }

        warnings.check(path);
        check_cancelled(stop);

        return result;
    }();

    auto result = read_luma(native, path, options, stop);
    check_cancelled(stop);

    return result;
}

}
