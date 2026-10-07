#include <GhidraEngine/hash/pdq.hpp>
#include <GhidraEngine/match/video.hpp>
#include <GhidraEngine/video/fingerprint.hpp>

#include "../../src/hdr.hpp"
#include "../../src/video/seek.hpp"
#include "../support/test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <limits>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;
static_assert(!std::is_copy_constructible_v<VideoDecoder>);
static_assert(std::is_nothrow_move_constructible_v<VideoDecoder>);

std::vector<Timestamp> timestamps(const std::filesystem::path &path, Timestamp interval = {}) {
    VideoDecodeOptions options;
    options.sample_interval = interval;
    VideoDecoder decoder(path, options);
    std::vector<Timestamp> result;

    while (auto frame = decoder.next()) {
        result.push_back(frame->timestamp);
    }

    return result;
}

TEST(VideoDecode, VfrFramesHaveFrozenTimestampsPixelsAndOwningStorage) {
    VideoDecoder decoder(fixture("video/vfr-alpha.mkv"));
    constexpr std::array<std::int64_t, 6> times{100000, 140000, 310000, 500000, 560000, 1000000};
    std::optional<DecodedVideoFrame> first;

    for (unsigned n = 0; n < times.size(); ++n) {
        auto frame = decoder.next();

        ASSERT_TRUE(frame);
        EXPECT_EQ(frame->timestamp, Timestamp{times[n]});
        ASSERT_EQ(frame->image.width, 64U);
        ASSERT_EQ(frame->image.height, 48U);
        ASSERT_EQ(frame->image.luma.size(), 64U * 48U);

        for (unsigned y = 0; y < 48; ++y) {
            for (unsigned x = 0; x < 64; ++x) {
                const unsigned alpha = x < 32 ? 255 : 128;
                const auto blend = [alpha](unsigned c) {
                    return ((c % 256) * alpha + 255 * (255 - alpha) + 127) / 255;
                };

                EXPECT_FLOAT_EQ(
                    frame->image.luma[y * 64 + x],
                    luma(blend(x + y * 2 + n * 13), blend(y * 5 + n * 11), blend(x * 3 + n * 7))
                );
            }
        }

        if (n == 0) {
            first = std::move(frame);
        }
    }

    EXPECT_FALSE(decoder.next());
    EXPECT_FALSE(decoder.next());
    ASSERT_TRUE(first);
    EXPECT_EQ(first->image.luma.front(), 0);

    VideoDecodeOptions black;
    black.alpha_background = {0, 0, 0};
    VideoDecoder other(fixture("video/vfr-alpha.mkv"), black);
    const auto frame = other.next();

    ASSERT_TRUE(frame);
    EXPECT_LT(frame->image.luma[32], first->image.luma[32]);
}

TEST(VideoDecode, SamplingUsesGapsAndPreservesNegativePresentationOrigin) {
    const auto path = fixture("video/vfr-alpha.mkv");

    EXPECT_EQ(
        timestamps(path, Timestamp{200000}),
        (std::vector<Timestamp>{
            Timestamp{100000},
            Timestamp{310000},
            Timestamp{560000},
            Timestamp{1000000}
        })
    );
    EXPECT_EQ(
        timestamps(path, Timestamp{250000}),
        (std::vector<Timestamp>{Timestamp{100000}, Timestamp{500000}, Timestamp{1000000}})
    );
    EXPECT_EQ(
        timestamps(path, Timestamp{500000}),
        (std::vector<Timestamp>{Timestamp{100000}, Timestamp{1000000}})
    );

    const auto negative = timestamps(fixture("video/negative.ts"));

    ASSERT_EQ(negative.size(), 10U);

    for (unsigned i = 0; i < 10; ++i) {
        EXPECT_EQ(negative[i], Timestamp{static_cast<std::int64_t>(i) * 40000 - 200000});
    }

    EXPECT_EQ(
        timestamps(fixture("video/negative.ts"), Timestamp{250000}),
        (std::vector<Timestamp>{Timestamp{-200000}, Timestamp{80000}})
    );

    const auto reordered = timestamps(fixture("video/display-0.mp4"));

    ASSERT_EQ(reordered.size(), 10U);

    for (unsigned i = 0; i < 10; ++i) {
        EXPECT_EQ(reordered[i], Timestamp{i * 40000});
    }
}

TEST(VideoDecode, SeekOriginsBoundsAndReadingAfterEof) {
    TempDirectory directory;
    const auto path = directory.path() / "bytes";
    write_file(path, "0123456789");
    std::ifstream input(path, std::ios::binary);
    const auto seek = [&](std::int64_t offset, int origin) {
        return detail::seek_video_input(input, 10, offset, origin);
    };

    EXPECT_EQ(seek(4, SEEK_SET), 4);
    EXPECT_EQ(seek(2, SEEK_CUR), 6);
    EXPECT_EQ(seek(-3, SEEK_END), 7);
    EXPECT_EQ(input.get(), '7');
    EXPECT_EQ(seek(-2, SEEK_CUR), 6);
    EXPECT_EQ(seek(0, SEEK_END), 10);
    EXPECT_EQ(input.get(), std::char_traits<char>::eof());
    EXPECT_EQ(seek(1, SEEK_SET), 1);
    EXPECT_EQ(input.get(), '1');
    EXPECT_EQ(seek(-1, SEEK_SET), -1);
    EXPECT_EQ(seek(1, SEEK_END), -1);
    EXPECT_EQ(seek(std::numeric_limits<std::int64_t>::min(), SEEK_CUR), -1);
    EXPECT_EQ(seek(std::numeric_limits<std::int64_t>::max(), SEEK_CUR), -1);
    EXPECT_EQ(seek(0, -1), -1);
}

TEST(VideoDecode, Mp4MetadataBeyondAvioBufferMatchesOriginalFrames) {
    const auto original = fixture("video/display-0.mp4");
    std::ifstream input(original, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    std::size_t moov = 0;

    while (moov + 8 <= bytes.size() && bytes.compare(moov + 4, 4, "moov") != 0) {
        std::uint32_t size = 0;

        for (unsigned i = 0; i < 4; ++i) {
            size = (size << 8) | static_cast<unsigned char>(bytes[moov + i]);
        }

        ASSERT_GE(size, 8U);
        ASSERT_LE(size, bytes.size() - moov);

        moov += size;
    }

    ASSERT_LE(moov + 8, bytes.size());

    std::string free(131072, '\0');
    free[1] = 2; // Big-endian 131072-byte free box.
    free.replace(4, 4, "free");
    TempDirectory directory;
    const auto enlarged = directory.path() / "large.mp4";
    write_file(enlarged, bytes.substr(0, moov) + free + bytes.substr(moov));
    VideoDecoder base(original), other(enlarged);
    unsigned count = 0;

    while (auto expected = base.next()) {
        const auto actual = other.next();

        ASSERT_TRUE(actual);
        EXPECT_EQ(actual->timestamp, expected->timestamp);
        EXPECT_EQ(actual->image.width, expected->image.width);
        EXPECT_EQ(actual->image.height, expected->image.height);
        EXPECT_EQ(actual->image.luma, expected->image.luma);

        ++count;
    }

    EXPECT_EQ(count, 10U);
    EXPECT_FALSE(other.next());
}

class VideoOrientationTest : public testing::TestWithParam<unsigned> {};

TEST_P(VideoOrientationTest, DisplayMatrixPreservesTransformedPixels) {
    VideoDecoder base(fixture("video/display-0.mp4"));
    const auto original = base.next();

    ASSERT_TRUE(original);

    const auto &image = original->image;
    const auto w = image.width, h = image.height;
    const unsigned orientation = GetParam();
    VideoDecoder decoder(fixture("video/display-" + std::to_string(orientation) + ".mp4"));
    const auto frame = decoder.next();

    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->image.width, orientation % 2 ? h : w);
    ASSERT_EQ(frame->image.height, orientation % 2 ? w : h);

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::array positions{
                y * w + x,
                x * h + h - 1 - y,
                (h - 1 - y) * w + w - 1 - x,
                (w - 1 - x) * h + y
            };
            auto target = positions[orientation % 4];

            if (orientation >= 4) {
                target = target / frame->image.width * frame->image.width + frame->image.width - 1 -
                         target % frame->image.width;
            }

            EXPECT_FLOAT_EQ(frame->image.luma[target], image.luma[y * w + x]);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Display, VideoOrientationTest, testing::Range(0U, 8U));

TEST(VideoDecode, InvalidConfigurationAndPixelBudgetsAreChecked) {
    const auto path = fixture("video/vfr-alpha.mkv");
    VideoDecodeOptions options;

    for (auto limit : {std::size_t{0}, std::numeric_limits<std::size_t>::max()}) {
        options = {};
        options.max_pixels = limit;

        EXPECT_THROW((void)VideoDecoder(path, options), std::invalid_argument);

        options = {};
        options.max_dimension = limit;

        EXPECT_THROW((void)VideoDecoder(path, options), std::invalid_argument);
    }

    options = {};
    options.sample_interval = Timestamp{-1};

    EXPECT_THROW((void)VideoDecoder(path, options), std::invalid_argument);

    options = {};
    options.max_dimension = 63;

    EXPECT_THROW((void)VideoDecoder(path, options), std::filesystem::filesystem_error);

    VideoDecoder raw(fixture("video/parser-pts.m4v"), options);

    EXPECT_THROW((void)raw.next(), std::filesystem::filesystem_error);
    EXPECT_THROW((void)raw.next(), std::logic_error);

    options.max_dimension = 64;
    options.max_pixels = 3071;

    EXPECT_THROW((void)VideoDecoder(path, options), std::filesystem::filesystem_error);

    options.max_pixels = 3072;
    VideoDecoder exact(path, options);

    EXPECT_TRUE(exact.next());
}

TEST(VideoDecode, UnsupportedMetadataFailsAndFailedInstancesRejectFurtherReads) {
    for (const auto *name : {"missing-pts.mkv", "display-8.mp4"}) {
        const auto path = fixture("video/" + std::string(name));
        VideoDecoder decoder(path);

        try {
            (void)decoder.next();
            FAIL() << name;
        } catch (const std::filesystem::filesystem_error &error) {
            EXPECT_EQ(error.path1(), path);
        }

        EXPECT_THROW((void)decoder.next(), std::logic_error);
    }

    EXPECT_THROW((void)VideoDecoder(fixture("video/audio.wav")), std::filesystem::filesystem_error);

    TempDirectory directory;
    write_file(directory.path() / "empty", "");
    write_file(directory.path() / "garbage", "not a container");

    for (const auto &path :
         {directory.path(),
          directory.path() / "empty",
          directory.path() / "garbage",
          directory.path() / "absent"}) {
        EXPECT_THROW((void)VideoDecoder(path), std::filesystem::filesystem_error);
    }
}

TEST(VideoDecode, HdrTransferFunctionsHaveReferenceValuesAndToneMappingIsBounded) {
    EXPECT_NEAR(detail::pq_nits(0), 0, 1e-9);
    EXPECT_NEAR(detail::pq_nits(0.5080784215), 100, 0.00001);
    EXPECT_NEAR(detail::pq_nits(1), 10000, 0.00001);
    EXPECT_NEAR(detail::hlg_linear(0.5), 1.0 / 12.0, 1e-9);
    EXPECT_NEAR(detail::hlg_linear(1), 1, 0.00001);
    EXPECT_EQ(detail::tone_map({0, 0, 0}, {}), (std::array<unsigned, 3>{0, 0, 0}));

    const auto white = 1000.0 / 203.0;

    EXPECT_EQ(
        detail::tone_map({white, white, white}, {}),
        (std::array<unsigned, 3>{255, 255, 255})
    );
}

TEST(VideoDecode, PqAndHlgVideosDecodeAndFingerprintWithConfigurableExposure) {
    for (const auto *name : {"hdr.mp4", "hdr10.mkv", "hlg.mkv"}) {
        const auto path = fixture("video/" + std::string(name));
        VideoDecoder decoder(path);
        const auto frame = decoder.next();

        ASSERT_TRUE(frame) << name;
        EXPECT_TRUE(std::ranges::all_of(frame->image.luma, [](float value) {
            return std::isfinite(value) && value >= 0 && value <= 255;
        }));

        VideoDecodeOptions brighter;
        brighter.hdr.exposure = 2;
        VideoDecoder other(path, brighter);
        const auto changed = other.next();

        ASSERT_TRUE(changed);
        EXPECT_NE(frame->image.luma, changed->image.luma);
        EXPECT_FALSE(fingerprint_video(path).frames.empty());

        VideoDecodeOptions invalid;
        invalid.hdr.exposure = std::numeric_limits<double>::quiet_NaN();
        EXPECT_THROW((void)VideoDecoder(path, invalid), std::invalid_argument);
    }
}

TEST(VideoDecode, CorruptPacketsAndTruncatedMp4FailButNativeMatroskaEofHasKnownLimit) {
    TempDirectory directory;
    const auto mp4 = directory.path() / "truncated.mp4";
    std::filesystem::copy_file(fixture("video/display-0.mp4"), mp4);
    std::filesystem::resize_file(mp4, 1000);

    EXPECT_THROW((void)timestamps(mp4), std::filesystem::filesystem_error);

    const auto raw = directory.path() / "corrupt.m4v";
    std::filesystem::copy_file(fixture("video/parser-pts.m4v"), raw);
    std::fstream output(raw, std::ios::binary | std::ios::in | std::ios::out);
    output.seekp(100);
    const std::array<char, 500> zeros{};
    output.write(zeros.data(), zeros.size());
    output.close();

    EXPECT_THROW((void)timestamps(raw), std::filesystem::filesystem_error);

    const auto mkv = directory.path() / "partial.mkv";
    std::filesystem::copy_file(fixture("video/vfr-alpha.mkv"), mkv);
    std::filesystem::resize_file(mkv, 4000);
    // FFmpeg can report ordinary EOF after a logged truncation; completeness is not certified.
    EXPECT_LT(timestamps(mkv).size(), 6U);
}

TEST(VideoDecode, MovePreservesIoCallbacksAndCancellationInvalidatesInstance) {
    std::stop_source stop;
    VideoDecoder original(fixture("video/vfr-alpha.mkv"), {}, stop.get_token());

    ASSERT_TRUE(original.next());

    auto moved = std::move(original);

    EXPECT_THROW((void)original.next(), std::logic_error);

    auto frame = moved.next();

    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->timestamp, Timestamp{140000});

    VideoDecoder assigned(fixture("video/display-0.mp4"));
    assigned = std::move(moved);
    frame = assigned.next();

    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->timestamp, Timestamp{310000});

    stop.request_stop();

    try {
        (void)assigned.next();
        FAIL();
    } catch (const std::system_error &error) {
        EXPECT_EQ(error.code(), std::errc::operation_canceled);
    }

    EXPECT_THROW((void)assigned.next(), std::logic_error);
    EXPECT_THROW(
        (void)VideoDecoder(fixture("video/missing"), {}, stop.get_token()),
        std::system_error
    );
}

TEST(VideoDecode, UnicodeWrongExtensionsAreContentProbedAndHdLumaIsFinite) {
    TempDirectory directory;
    const auto path = directory.path() / u8"видео.jpg[seek=3]";
    std::filesystem::copy_file(fixture("video/vfr-alpha.mkv"), path);

    EXPECT_EQ(timestamps(path).size(), 6U);

    auto nul = path.native();
    nul.push_back(0);

    EXPECT_THROW((void)VideoDecoder(std::filesystem::path(nul)), std::filesystem::filesystem_error);

    VideoDecodeOptions options;
    options.max_pixels = 1920U * 1080U;
    options.max_dimension = 1920;
    VideoDecoder hd(fixture("video/hd.mkv"), options);
    const auto frame = hd.next();

    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->image.width, 1920U);
    EXPECT_EQ(frame->image.height, 1080U);
    EXPECT_TRUE(std::ranges::all_of(frame->image.luma, [](float v) {
        return std::isfinite(v) && v >= 0 && v <= 255;
    }));
}

TEST(VideoFingerprint, SelectedFramesPreserveDecodedPdqQualityAndTimestamps) {
    for (int interval : {0, 200000, 250000, 1000000}) {
        VpdqGenerationOptions options;
        options.decode.sample_interval = Timestamp{interval};
        const auto signature = fingerprint_video(fixture("video/vfr-alpha.mkv"), options);
        VideoDecoder decoder(fixture("video/vfr-alpha.mkv"), options.decode);
        std::size_t n = 0;

        while (auto frame = decoder.next()) {
            ASSERT_LT(n, signature.frames.size());

            const auto hash =
                compute_pdq(frame->image.luma, frame->image.width, frame->image.height);

            EXPECT_EQ(
                signature.frames[n++],
                (VpdqFrame{hash.hash, hash.quality, frame->timestamp})
            );
        }

        EXPECT_EQ(n, signature.frames.size());
    }

    VpdqGenerationOptions all;
    all.decode.sample_interval = {};
    const auto negative = fingerprint_video(fixture("video/negative.ts"), all);

    ASSERT_EQ(negative.frames.size(), 10U);
    EXPECT_EQ(negative.frames.front().timestamp, Timestamp{-200000});
    EXPECT_EQ(negative.frames.back().timestamp, Timestamp{160000});
    EXPECT_EQ(fingerprint_video(fixture("video/display-0.mp4"), all).frames.size(), 10U);
    EXPECT_EQ(fingerprint_video(fixture("video/vfr-alpha.mkv")).frames.size(), 1U);
}

TEST(VideoFingerprint, PruningUsesLastRetainedHashAndCanReduceRecall) {
    const VpdqSignature original{
        {{{}, PdqQuality{80}, Timestamp{-10}},
         {{}, PdqQuality{90}, {}},
         {prefix_hash(1), PdqQuality{75}, Timestamp{10}},
         {prefix_hash(2), PdqQuality{70}, Timestamp{20}},
         {{}, PdqQuality{60}, Timestamp{30}}}
    };

    EXPECT_EQ(
        prune_vpdq(original, PdqDistance{1}).frames,
        (std::vector<VpdqFrame>{original.frames[0], original.frames[3], original.frames[4]})
    );
    EXPECT_TRUE(prune_vpdq({}, {}).frames.empty());

    const VpdqSignature pair{{original.frames[0], original.frames[2]}};
    const VpdqSignature target{{original.frames[2]}};
    const VpdqMatchPolicy policy{PdqDistance{0}, PdqQuality{0}, Coverage{0}, Coverage{1}};

    EXPECT_TRUE(match_vpdq(VideoId{0}, pair, VideoId{1}, target, policy));
    EXPECT_FALSE(
        match_vpdq(VideoId{0}, prune_vpdq(pair, PdqDistance{1}), VideoId{1}, target, policy)
    );
}

TEST(VideoFingerprint, BudgetsCancellationAndConcurrentCallsAreDefined) {
    VpdqGenerationOptions options;
    options.max_frames = 0;
    const auto path = fixture("video/vfr-alpha.mkv");

    EXPECT_THROW((void)fingerprint_video(path, options), std::invalid_argument);

    options.max_frames = 1;
    options.decode.sample_interval = {};

    EXPECT_THROW((void)fingerprint_video(path, options), std::length_error);

    options.prune_distance = PdqDistance{256};

    EXPECT_EQ(fingerprint_video(path, options).frames.size(), 1U);

    options.decode.sample_interval = Timestamp{-1};

    EXPECT_THROW((void)fingerprint_video(path, options), std::invalid_argument);

    std::stop_source stop;
    stop.request_stop();

    EXPECT_THROW((void)fingerprint_video(path, {}, stop.get_token()), std::system_error);

    std::array<std::future<VpdqSignature>, 4> workers;

    for (auto &worker : workers) {
        worker = std::async(std::launch::async, [&] {
            return fingerprint_video(path);
        });
    }

    const auto expected = fingerprint_video(path);

    for (auto &worker : workers) {
        EXPECT_EQ(worker.get(), expected);
    }
}

} // namespace
