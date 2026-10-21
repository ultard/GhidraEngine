#include <GhidraEngine/hash/pdq.hpp>

#include "../../src/hdr.hpp"
#include "../support/test_support.hpp"

#include <gtest/gtest.h>
#include <vips/vips.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <limits>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

class ImageCodecTest : public testing::TestWithParam<const char *> {};

TEST_P(ImageCodecTest, LosslessRgbPixelsHaveFrozenLuma) {
    const auto image = decode_image(fixture(std::string("image/") + GetParam()));

    EXPECT_EQ(image.width, 3U);
    EXPECT_EQ(image.height, 2U);
    EXPECT_EQ(
        image.luma,
        (std::vector<float>{
            luma(255, 0, 0),
            luma(0, 255, 0),
            luma(0, 0, 255),
            luma(17, 33, 201),
            luma(255, 255, 255),
            0
        })
    );
}

INSTANTIATE_TEST_SUITE_P(
    Formats,
    ImageCodecTest,
    testing::Values("rgb8.png", "rgb8.webp", "rgb8.tiff", "rgb16.png")
);

class ImageOrientationTest : public testing::TestWithParam<unsigned> {};

TEST_P(ImageOrientationTest, ExifOrientationHasFrozenPixelOrder) {
    constexpr std::array<std::array<unsigned, 6>, 8> expected{
        {{20, 50, 80, 110, 140, 170},
         {80, 50, 20, 170, 140, 110},
         {170, 140, 110, 80, 50, 20},
         {110, 140, 170, 20, 50, 80},
         {20, 110, 50, 140, 80, 170},
         {110, 20, 140, 50, 170, 80},
         {170, 80, 140, 50, 110, 20},
         {80, 170, 50, 140, 20, 110}}
    };
    const auto orientation = GetParam();
    const auto image =
        decode_image(fixture("image/orientation-" + std::to_string(orientation) + ".png"));

    EXPECT_EQ(image.width, orientation < 5 ? 3U : 2U);
    EXPECT_EQ(image.height, orientation < 5 ? 2U : 3U);
    ASSERT_EQ(image.luma.size(), 6U);

    for (unsigned i = 0; i < 6; ++i) {
        const auto shade = expected[orientation - 1][i];

        EXPECT_FLOAT_EQ(image.luma[i], luma(shade, shade, shade));
    }
}

INSTANTIATE_TEST_SUITE_P(Exif, ImageOrientationTest, testing::Range(1U, 9U));

TEST(ImageDecode, JpegOrientationRotatesDecodedPixels) {
    const auto original = decode_image(fixture("image/bridge.jpg"));
    const auto rotated = decode_image(fixture("image/orientation-6.jpg"));

    ASSERT_EQ(rotated.width, original.height);
    ASSERT_EQ(rotated.height, original.width);

    for (std::size_t y = 0; y < rotated.height; ++y) {
        for (std::size_t x = 0; x < rotated.width; ++x) {
            EXPECT_EQ(
                rotated.luma[y * rotated.width + x],
                original.luma[(original.height - 1 - x) * original.width + y]
            );
        }
    }
}

TEST(ImageDecode, FastDecodeBoundsRgbPixelsAndPreservesOrientationAndLimits) {
    ImageDecodeOptions options;
    options.target_dimension = 256;
    const auto original = decode_image(fixture("image/bridge.jpg"), options);
    const auto rotated = decode_image(fixture("image/orientation-6.jpg"), options);

    EXPECT_LE(std::max(original.width, original.height), 256U);
    EXPECT_EQ(rotated.width, original.height);
    EXPECT_EQ(rotated.height, original.width);
    EXPECT_EQ(rotated.luma.size(), original.luma.size());

    const auto large = decode_image(fixture("image/fast-large.jpg"), options);
    EXPECT_EQ(large.width, 256U);
    EXPECT_EQ(large.height, 128U);
    EXPECT_EQ(decode_image(fixture("image/fast-large.jpg")).width, 2048U);

    const auto small = decode_image(fixture("image/rgb8.png"), options);
    EXPECT_EQ(small.luma, decode_image(fixture("image/rgb8.png")).luma);

    for (const auto *name : {"invalid-icc.png", "truncated.jpg", "oversized.png", "animated.gif"}) {
        EXPECT_THROW(
            (void)decode_image(fixture(std::string("image/") + name), options),
            std::filesystem::filesystem_error
        );
    }

    options.max_pixels = 6;
    EXPECT_THROW((void)decode_image(fixture("image/bridge.jpg"), options),
        std::filesystem::filesystem_error);
}

TEST(ImageDecode, ConcurrentFailuresDoNotRejectValidFastImages) {
    std::array<std::future<void>, 8> workers;
    ImageDecodeOptions options;
    options.target_dimension = 256;
    const auto expected = decode_image(fixture("image/bridge.jpg"), options).luma;

    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::async(std::launch::async, [&, i] {
            for (unsigned attempt = 0; attempt < 10; ++attempt) {
                if (i % 2 == 0) {
                    EXPECT_THROW((void)decode_image(fixture("image/truncated.jpg"), options),
                        std::filesystem::filesystem_error);
                } else {
                    EXPECT_EQ(decode_image(fixture("image/bridge.jpg"), options).luma, expected);
                }
            }
        });
    }
    for (auto &worker : workers) {
        worker.get();
    }
}

TEST(ImageDecode, NativeSinkPreservesLargeRgb8AndRgb16Pixels) {
    const TempDirectory directory;
    constexpr std::size_t width = 128;
    constexpr std::size_t height = 64;
    std::vector<float> expected;
    expected.reserve(width * height);

    for (const unsigned depth : {8U, 16U}) {
        std::string bytes = "P6\n128 64\n" + std::string(depth == 8 ? "255\n" : "65535\n");

        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const std::array<unsigned, 3> rgb{
                    static_cast<unsigned>((x * 3 + y * 5) % 256),
                    static_cast<unsigned>((x * 7 + y * 11) % 256),
                    static_cast<unsigned>((x * 13 + y * 17) % 256)
                };

                if (depth == 8) {
                    expected.push_back(luma(rgb[0], rgb[1], rgb[2]));
                }

                for (const auto channel : rgb) {
                    if (depth == 16) {
                        bytes.push_back(static_cast<char>(channel));
                    }
                    bytes.push_back(static_cast<char>(channel));
                }
            }
        }

        const auto path = directory.path() / ("rgb" + std::to_string(depth) + ".ppm");
        write_file(path, bytes);
        const auto image = decode_image(path);

        EXPECT_EQ(image.width, width);
        EXPECT_EQ(image.height, height);
        EXPECT_EQ(image.luma, expected);
    }
}

TEST(ImageDecode, CancellationDuringNativeSinkReturnsNoPartialImage) {
    (void)decode_image(fixture("image/rgb8.png"));
    const auto signal = g_signal_lookup("eval", vips_image_get_type());
    ASSERT_NE(signal, 0U);
    std::stop_source stop;
    const auto hook = g_signal_add_emission_hook(
        signal,
        0,
        [](GSignalInvocationHint *, guint, const GValue *, gpointer context) -> gboolean {
            static_cast<std::stop_source *>(context)->request_stop();

            return TRUE;
        },
        &stop,
        nullptr
    );
    ASSERT_NE(hook, 0U);

    struct RemoveHook {
        guint signal;
        gulong hook;

        ~RemoveHook() {
            g_signal_remove_emission_hook(signal, hook);
        }
    } cleanup{signal, hook};

    try {
        (void)decode_image(fixture("image/fast-large.jpg"), {}, stop.get_token());
        FAIL() << "Native evaluation returned an image after cancellation";
    } catch (const std::system_error &error) {
        EXPECT_EQ(error.code(), std::errc::operation_canceled);
    }

    EXPECT_TRUE(stop.stop_requested());
    EXPECT_EQ(decode_image(fixture("image/rgb8.png")).luma.size(), 6U);
}

TEST(ImageDecode, AlphaAndUnsigned16BitSamplesUseDefinedRounding) {
    EXPECT_EQ(
        decode_image(fixture("image/rgba8.png")).luma,
        (std::vector<float>{luma(255, 0, 0), luma(127, 255, 127), luma(255, 255, 255)})
    );

    ImageDecodeOptions black;
    black.alpha_background = {0, 0, 0};

    EXPECT_EQ(
        decode_image(fixture("image/rgba8.png"), black).luma,
        (std::vector<float>{luma(255, 0, 0), luma(0, 128, 0), 0})
    );

    const std::vector<float> expected{luma(128, 0, 0), luma(0, 255, 0), 0};

    EXPECT_EQ(decode_image(fixture("image/rgba16.png"), black).luma, expected);
    EXPECT_EQ(decode_image(fixture("image/associated-alpha.tiff"), black).luma, expected);
    EXPECT_EQ(
        decode_image(fixture("image/gray-alpha16.png"), black).luma,
        (std::vector<float>{luma(128, 128, 128), 0})
    );

    const auto gray = decode_image(fixture("image/gray16.png"));
    const std::array shades{0U, 1U, 128U, 255U, 100U, 156U};

    ASSERT_EQ(gray.luma.size(), shades.size());

    for (unsigned i = 0; i < shades.size(); ++i) {
        EXPECT_FLOAT_EQ(gray.luma[i], luma(shades[i], shades[i], shades[i]));
    }
}

TEST(ImageDecode, IccProfilesConvertToSrgbAndMalformedProfilesFail) {
    const auto linear = decode_image(fixture("image/linear-icc.png"));

    ASSERT_EQ(linear.luma.size(), 6U);
    EXPECT_NEAR(linear.luma[3], luma(73, 101, 230), 1.0F);

    const auto plain = decode_image(fixture("image/rgb8.png"));
    const auto srgb = decode_image(fixture("image/srgb-icc.png"));

    ASSERT_EQ(srgb.luma.size(), plain.luma.size());

    for (unsigned i = 0; i < plain.luma.size(); ++i) {
        EXPECT_NEAR(srgb.luma[i], plain.luma[i], 1.0F);
    }

    EXPECT_THROW(
        (void)decode_image(fixture("image/invalid-icc.png")),
        std::filesystem::filesystem_error
    );
}

class InvalidImageTest : public testing::TestWithParam<const char *> {};

TEST_P(InvalidImageTest, ReportsInputPathAndLeavesDecoderUsable) {
    const auto path = fixture(std::string("image/") + GetParam());

    try {
        (void)decode_image(path);
        FAIL() << "Invalid image accepted";
    } catch (const std::filesystem::filesystem_error &error) {
        EXPECT_EQ(error.path1(), path);
    }

    EXPECT_EQ(decode_image(fixture("image/rgb8.png")).luma.size(), 6U);
}

INSTANTIATE_TEST_SUITE_P(
    Invalid,
    InvalidImageTest,
    testing::Values(
        "truncated.png",
        "bad-crc.png",
        "truncated.jpg",
        "animated.gif",
        "multipage.tiff",
        "hdr-nan.tiff",
        "hdr-bad-alpha.tiff",
        "oversized.png"
    )
);

TEST(ImageDecode, LimitsRejectInvalidSettingsAndAcceptExactPixelBudget) {
    const auto path = fixture("image/rgb8.png");
    ImageDecodeOptions options;
    options.max_pixels = 5;

    EXPECT_THROW((void)decode_image(path, options), std::filesystem::filesystem_error);

    options.max_pixels = 6;

    EXPECT_EQ(decode_image(path, options).luma.size(), 6U);

    options.max_dimension = 2;

    EXPECT_THROW((void)decode_image(path, options), std::filesystem::filesystem_error);

    for (auto limit : {std::size_t{0}, std::numeric_limits<std::size_t>::max()}) {
        options = {};
        options.max_pixels = limit;

        EXPECT_THROW((void)decode_image(path, options), std::invalid_argument);

        options = {};
        options.max_dimension = limit;

        EXPECT_THROW((void)decode_image(path, options), std::invalid_argument);
    }
}

TEST(ImageDecode, ContentProbingUnicodePathsAndReplacedFilesWork) {
    TempDirectory directory;
    const auto path = directory.path() / u8"изображение.jpg[shrink=8]";
    std::filesystem::copy_file(fixture("image/rgb8.png"), path);

    EXPECT_EQ(decode_image(path).luma, decode_image(fixture("image/rgb8.png")).luma);

    std::filesystem::copy_file(
        fixture("image/rgba8.png"),
        path,
        std::filesystem::copy_options::overwrite_existing
    );

    EXPECT_EQ(decode_image(path).height, 1U);

    auto nul = path.native();
    nul.push_back(0);

    EXPECT_THROW((void)decode_image(std::filesystem::path(nul)), std::filesystem::filesystem_error);

    write_file(directory.path() / "empty", "");

    for (const auto &invalid :
         {directory.path(), directory.path() / "missing", directory.path() / "empty"}) {
        EXPECT_THROW((void)decode_image(invalid), std::filesystem::filesystem_error);
    }
}

TEST(ImageDecode, CancellationAndConcurrentErrorsStayIndependent) {
    std::stop_source stop;
    stop.request_stop();

    try {
        (void)decode_image(fixture("image/missing"), {}, stop.get_token());
        FAIL();
    } catch (const std::system_error &error) {
        EXPECT_EQ(error.code(), std::errc::operation_canceled);
    }

    std::array<std::future<std::vector<float>>, 4> workers;

    for (auto &worker : workers) {
        worker = std::async(std::launch::async, [] {
            EXPECT_THROW(
                (void)decode_image(fixture("image/truncated.png")),
                std::filesystem::filesystem_error
            );

            return decode_image(fixture("image/rgb8.png")).luma;
        });
    }

    const auto expected = decode_image(fixture("image/rgb8.png")).luma;

    for (auto &worker : workers) {
        EXPECT_EQ(worker.get(), expected);
    }
}

TEST(ImageDecode, PhotoPixelsAndPdqMatchFrozenRawFixtures) {
    for (auto name : {"bridge", "pen"}) {
        const auto image = decode_image(fixture("image/" + std::string(name) + ".png"));
        const auto raw_path = fixture(
            "pdq/" + std::string(name) + "-" + std::to_string(image.width) + "x" +
            std::to_string(image.height) + ".rgb"
        );
        std::ifstream raw(raw_path, std::ios::binary);

        ASSERT_TRUE(raw);

        std::vector<float> expected;
        std::array<unsigned char, 3> rgb{};

        while (raw.read(reinterpret_cast<char *>(rgb.data()), 3)) {
            expected.push_back(luma(rgb[0], rgb[1], rgb[2]));
        }

        EXPECT_FALSE(raw.bad());
        EXPECT_EQ(image.luma, expected);

        const auto hash = compute_pdq(image.luma, image.width, image.height);

        EXPECT_EQ(hash.quality.value(), 100);
        EXPECT_EQ(
            encode_pdq(hash.hash),
            std::string_view(name) == "bridge"
                ? "78fcf0cee4f4a8478e370a22138f63f4b36e26d592223e1932e6b39c4e9c9b22"
                : "1fc11b9d267fbc6691c0c3f30e040f9df69b836303e10f077fcdfc12c02d01f9"
        );
    }
}

TEST(ImageDecode, FloatAndRadianceHdrImagesPreserveHighlightsAndAlpha) {
    const auto image = decode_image(fixture("image/hdr-float.tiff"));

    ASSERT_EQ(image.luma.size(), 4U);
    EXPECT_EQ(image.luma[0], 0);
    EXPECT_LT(image.luma[1], image.luma[2]);
    EXPECT_LT(image.luma[2], 255);

    const auto red = detail::tone_map({1, 0, 0}, {});

    EXPECT_FLOAT_EQ(image.luma[3], luma((red[0] + 256) / 2, 128, 128));

    ImageDecodeOptions black;
    black.alpha_background = {0, 0, 0};
    EXPECT_LT(decode_image(fixture("image/hdr-float.tiff"), black).luma[3], image.luma[3]);

    const auto radiance = decode_image(fixture("image/hdr-radiance.hdr"));

    ASSERT_EQ(radiance.luma.size(), 3U);
    EXPECT_EQ(radiance.luma[0], 0);
    EXPECT_LT(radiance.luma[1], radiance.luma[2]);
    const auto gray = decode_image(fixture("image/float.tiff"));

    EXPECT_TRUE(std::ranges::all_of(gray.luma, [](float value) {
        return std::isfinite(value) && value >= 0 && value <= 255;
    }));

    ImageDecodeOptions brighter;
    brighter.hdr.exposure = 2;
    EXPECT_GT(decode_image(fixture("image/hdr-float.tiff"), brighter).luma[1], image.luma[1]);

    brighter.hdr.exposure = -1;
    EXPECT_THROW(
        (void)decode_image(fixture("image/hdr-float.tiff"), brighter),
        std::invalid_argument
    );
}

} // namespace
