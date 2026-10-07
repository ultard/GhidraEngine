#include <GhidraEngine/hash/blake3.hpp>
#include <GhidraEngine/hash/pdq.hpp>

#include "../../src/hash/pdq_internal.hpp"
#include "../support/blake3_vectors.hpp"
#include "../support/test_support.hpp"

#include <gtest/gtest.h>
#include <pdq/cpp/hashing/pdqhashing.h>

#include <array>
#include <charconv>
#include <future>
#include <limits>
#include <sstream>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

std::string byte_pattern(std::size_t count) {
    std::string bytes(count, '\0');

    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<char>(i % 251);
    }

    return bytes;
}

Blake3Digest digest_from_hex(std::string_view hex) {
    Blake3Digest digest;

    for (std::size_t i = 0; i < digest.bytes.size(); ++i) {
        unsigned value = 0;
        const auto *first = hex.data() + i * 2;
        const auto parsed = std::from_chars(first, first + 2, value, 16);

        if (parsed.ec != std::errc{} || parsed.ptr != first + 2) {
            throw std::runtime_error("Malformed BLAKE3 reference vector");
        }

        digest.bytes[i] = static_cast<std::uint8_t>(value);
    }

    return digest;
}

class Blake3VectorTest : public testing::TestWithParam<std::size_t> {};

TEST_P(Blake3VectorTest, BytesStreamAndFileMatchOfficialDigest) {
    const auto &vector = blake3_vectors[GetParam()];
    const auto bytes = byte_pattern(vector.input_length);
    const auto expected = digest_from_hex(vector.digest);

    EXPECT_EQ(hash_blake3(std::as_bytes(std::span(bytes))), expected);

    for (auto mask : {std::ios::goodbit, std::ios::badbit | std::ios::failbit | std::ios::eofbit}) {
        std::istringstream stream(bytes);
        stream.exceptions(mask);

        EXPECT_EQ(hash_blake3_stream(stream), expected);
        EXPECT_EQ(stream.exceptions(), mask);
        EXPECT_TRUE(stream.eof());
    }

    TempDirectory directory;
    const auto path = directory.path() / u8"данные.jpg";
    write_file(path, bytes);

    EXPECT_EQ(hash_blake3_file(path), expected);
}

INSTANTIATE_TEST_SUITE_P(
    Official,
    Blake3VectorTest,
    testing::Range(std::size_t{0}, blake3_vectors.size()),
    [](const auto &info) {
        return "Bytes" + std::to_string(blake3_vectors[info.param].input_length);
    }
);

// No seeking or full-input allocation; errors and cancellation occur at a known read.
class TestStreamBuffer : public std::streambuf {
public:
    std::size_t remaining = 131073;
    std::size_t position = 0;
    unsigned reads = 0;
    std::streamsize largest_read = 0;
    bool fail_second_read = false;
    std::stop_source *cancel = nullptr;

protected:
    std::streamsize xsgetn(char *destination, std::streamsize requested) override {
        if (fail_second_read && reads == 1) {
            throw std::runtime_error("Injected I/O failure");
        }

        ++reads;
        largest_read = std::max(largest_read, requested);
        const auto count = std::min(remaining, static_cast<std::size_t>(requested));

        for (std::size_t i = 0; i < count; ++i) {
            destination[i] = static_cast<char>((position + i) % 251);
        }

        position += count;
        remaining -= count;

        if (cancel) {
            cancel->request_stop();
        }

        return static_cast<std::streamsize>(count);
    }
};

TEST(Blake3, NonSeekableStreamsRespectReadBoundaries) {
    for (std::size_t count : {65535U, 65536U, 65537U, 131072U, 131073U}) {
        SCOPED_TRACE(count);
        TestStreamBuffer buffer;
        buffer.remaining = count;
        std::istream stream(&buffer);
        const auto bytes = byte_pattern(count);

        EXPECT_EQ(hash_blake3_stream(stream), hash_blake3(std::as_bytes(std::span(bytes))));
        EXPECT_LE(buffer.largest_read, 65536);
    }

    std::istringstream stream("prefix" + byte_pattern(1025));
    stream.seekg(6);
    const auto suffix = byte_pattern(1025);

    EXPECT_EQ(hash_blake3_stream(stream), hash_blake3(std::as_bytes(std::span(suffix))));
}

TEST(Blake3, ReadErrorsPreserveExceptionMaskAndNeverReturnPartialDigest) {
    for (auto mask : {std::ios::goodbit, std::ios::badbit | std::ios::failbit}) {
        TestStreamBuffer buffer;
        buffer.fail_second_read = true;
        std::istream stream(&buffer);
        stream.exceptions(mask);

        EXPECT_THROW((void)hash_blake3_stream(stream), std::ios_base::failure);
        EXPECT_EQ(buffer.reads, 1U);
        EXPECT_TRUE(stream.bad());
        EXPECT_EQ(stream.exceptions(), mask);
    }

    for (auto state : {std::ios::failbit, std::ios::badbit, std::ios::eofbit}) {
        std::istringstream stream("bytes");
        stream.setstate(state);

        EXPECT_THROW((void)hash_blake3_stream(stream), std::ios_base::failure);
    }
}

TEST(Blake3, CancellationIsCheckedBeforeAndAfterReading) {
    for (bool pre_cancel : {false, true}) {
        TestStreamBuffer buffer;
        std::stop_source source;
        buffer.cancel = &source;

        if (pre_cancel) {
            source.request_stop();
        }

        std::istream stream(&buffer);

        try {
            (void)hash_blake3_stream(stream, source.get_token());
            FAIL() << "Cancellation ignored";
        } catch (const std::system_error &error) {
            EXPECT_EQ(error.code(), std::errc::operation_canceled);
        }

        EXPECT_EQ(buffer.reads, pre_cancel ? 0U : 1U);
    }
}

TEST(Blake3, InvalidFilePathsAndPreCancellationReportCorrectErrors) {
    TempDirectory directory;
    const auto path = directory.path() / "file";
    write_file(path, "content");
    auto nul = path.native();
    nul.push_back(0);
    nul += std::filesystem::path("suffix").native();

    for (const auto &invalid :
         {std::filesystem::path{},
          std::filesystem::path(nul),
          directory.path(),
          directory.path() / "missing"}) {
        try {
            (void)hash_blake3_file(invalid);
            FAIL() << "Invalid file accepted";
        } catch (const std::filesystem::filesystem_error &error) {
            EXPECT_EQ(error.path1(), invalid);
        }
    }

    std::stop_source source;
    source.request_stop();

    try {
        (void)hash_blake3_file(directory.path() / "missing", source.get_token());
        FAIL();
    } catch (const std::system_error &error) {
        EXPECT_EQ(error.code(), std::errc::operation_canceled);
    }
}

TEST(Blake3, FileSymlinksFollowTheTarget) {
    TempDirectory directory;
    write_file(directory.path() / "original", "content");
    std::error_code error;
    std::filesystem::create_symlink(
        directory.path() / "original",
        directory.path() / "alias",
        error
    );

    if (error) {
        GTEST_SKIP() << "Symlink creation unavailable: " << error.message();
    }

    EXPECT_EQ(
        hash_blake3_file(directory.path() / "alias"),
        hash_blake3_file(directory.path() / "original")
    );
}

TEST(Pdq, HexEncodingHasFrozenBitOrderAndRejectsMalformedInput) {
    const PdqHash hash{
        {0x0123456789abcdefULL, 0xfedcba9876543210ULL, 0x0001000200030004ULL, 0x8000000000000001ULL}
    };
    const std::string expected = "80000000000000010001000200030004fedcba98765432100123456789abcdef";

    EXPECT_EQ(encode_pdq(hash), expected);
    EXPECT_EQ(decode_pdq(expected), hash);
    EXPECT_EQ(decode_pdq(std::string(64, 'F')), prefix_hash(256));

    for (unsigned bit = 0; bit < 256; ++bit) {
        PdqHash single;
        single.words[bit / 64] = std::uint64_t{1} << (bit % 64);
        std::string text(64, '0');
        text[63 - bit / 4] = "1248"[bit % 4];

        EXPECT_EQ(encode_pdq(single), text);
        EXPECT_EQ(decode_pdq(text), single);
        EXPECT_EQ(pdq_distance({}, single).value(), 1);
        EXPECT_EQ(pdq_distance(prefix_hash(256), single).value(), 255);
    }

    for (auto text : {std::string{}, std::string(63, '0'), std::string(65, '0'), "0x" + expected}) {
        EXPECT_THROW((void)decode_pdq(text), std::invalid_argument);
    }

    for (unsigned position = 0; position < 64; ++position) {
        for (char bad : {'g', ' ', '\n', '\0', '+'}) {
            auto text = expected;
            text[position] = bad;

            EXPECT_THROW((void)decode_pdq(text), std::invalid_argument);
        }
    }
}

TEST(Pdq, RandomHashRoundtripsAndDistancesMatchIndependentBitOracle) {
    std::mt19937_64 random(0x706471);

    for (unsigned i = 0; i < 512; ++i) {
        const auto a = random_hash(random), b = random_hash(random);

        EXPECT_EQ(decode_pdq(encode_pdq(a)), a);
        EXPECT_EQ(pdq_distance(a, b).value(), bit_distance(a, b));
        EXPECT_EQ(pdq_distance(a, b), pdq_distance(b, a));
        EXPECT_EQ(pdq_distance(a, a).value(), 0);
    }
}

PdqFingerprint reference_pdq(const DecodedImage &image) {
    auto input = image.luma;
    std::vector<float> scratch(input.size());
    float b64[64][64], b16x64[16][64], b16[16][16];
    facebook::pdq::hashing::Hash256 hash;
    int quality = 0;
    facebook::pdq::hashing::pdqHash256FromFloatLuma(
        input.data(),
        scratch.data(),
        static_cast<int>(image.height),
        static_cast<int>(image.width),
        b64,
        b16x64,
        b16,
        hash,
        quality
    );
    PdqFingerprint result{{}, PdqQuality{quality}};

    for (unsigned bit = 0; bit < 256; ++bit) {
        if (hash.getBit(static_cast<int>(bit))) {
            result.hash.words[bit / 64] |= std::uint64_t{1} << (bit % 64);
        }
    }

    return result;
}

TEST(Pdq, FrozenLumaVectorsAndUpstreamAdapterAgreeWithoutMutatingInput) {
    struct Vector {
        unsigned width, height;
        const char *hex;
    };

    for (const auto &v :
         {Vector{5, 5, "9f276e0f60d990a48b626f5b9b2690b464d93e1b9f273b5a60d991a49b623a1e"},
          Vector{7, 9, "4f61399cca95ca77638d6c79cb2796d234d893869c72658b34cac67392963a94"},
          Vector{64, 64, "1503758f157735891de02eb517d72bbf9555c0155dd7d41554b7d185080ad5b3"},
          Vector{75, 128, "54e10d7054e1b2ede1c7a2b0f7c6a0b1e34c95da1cf768a394f529a3f4f52922"},
          Vector{193, 65, "5d5a610f5555880fcce1662c4087e6024cc5e222b7b31554bf76199fb7fe151f"}}) {
        SCOPED_TRACE(v.hex);
        const auto image = pattern_image(v.width, v.height);
        const auto before = image.luma;
        const auto actual = compute_pdq(image.luma, image.width, image.height);

        EXPECT_EQ(encode_pdq(actual.hash), v.hex);
        EXPECT_EQ(actual.quality.value(), 100);
        EXPECT_EQ(actual, reference_pdq(image));
        EXPECT_EQ(image.luma, before);
    }

    std::mt19937_64 random(42);

    for (unsigned i = 0; i < 32; ++i) {
        auto image = pattern_image(1 + random() % 200, 1 + random() % 150);

        for (auto &pixel : image.luma) {
            pixel = static_cast<float>(random() % 65536) / 257.0F;
        }

        EXPECT_EQ(compute_pdq(image.luma, image.width, image.height), reference_pdq(image));
    }
}

TEST(Pdq, ReusedWorkspaceMatchesReferenceAcrossChangingDimensions) {
    detail::PdqWorkspace workspace;

    for (const auto dimensions : {std::array<std::size_t, 2>{193, 65}, {64, 64}, {5, 7},
                                  {256, 128}, {193, 65}, {4, 4}}) {
        const auto image = pattern_image(dimensions[0], dimensions[1]);
        const auto before = image.luma;
        PdqFingerprint actual;
        detail::compute_pdq_variants(
            image.luma,
            image.width,
            image.height,
            std::span{&actual, 1},
            workspace
        );

        EXPECT_EQ(actual, reference_pdq(image));
        EXPECT_EQ(image.luma, before);
    }

    const auto input_capacity = workspace.input.capacity();
    const auto scratch_capacity = workspace.scratch.capacity();
    const auto image = pattern_image(193, 65);
    PdqFingerprint actual;
    detail::compute_pdq_variants(
        image.luma,
        image.width,
        image.height,
        std::span{&actual, 1},
        workspace
    );

    EXPECT_EQ(workspace.input.capacity(), input_capacity);
    EXPECT_EQ(workspace.scratch.capacity(), scratch_capacity);
    EXPECT_EQ(actual, reference_pdq(image));
}

TEST(Pdq, InvalidDimensionsAndSamplesFailBeforeProcessing) {
    const std::array<float, 25> pixels{};

    for (auto [w, h] :
         {std::pair<std::size_t, std::size_t>{0, 5},
          {5, 0},
          {5, 6},
          {std::numeric_limits<std::size_t>::max(), 2},
          {65536, 65536}}) {
        EXPECT_THROW((void)compute_pdq(pixels, w, h), std::invalid_argument);
    }

    for (float value :
         {-1.0F,
          256.0F,
          std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_THROW((void)compute_pdq(std::array{value}, 1, 1), std::invalid_argument);
    }

    for (auto [w, h] : {std::pair{1U, 1U}, {4U, 100U}, {100U, 4U}}) {
        EXPECT_EQ(compute_pdq(pattern_image(w, h).luma, w, h), PdqFingerprint{});
    }

    EXPECT_EQ(compute_pdq(std::vector<float>(4096, 127.0F), 64, 64).quality.value(), 0);

    std::vector<float> ramp(4096);

    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<float>((i % 64) * 4);
    }

    EXPECT_EQ(compute_pdq(ramp, 64, 64).quality.value(), 44);
}

TEST(Hashing, ConcurrentCallsHaveIndependentState) {
    TempDirectory directory;
    const auto path = directory.path() / "input";
    write_file(path, byte_pattern(131073));
    const auto image = pattern_image(75, 128);
    const auto expected = std::pair{hash_blake3_file(path), reference_pdq(image)};
    std::array<std::future<std::pair<Blake3Digest, PdqFingerprint>>, 4> workers;

    for (auto &worker : workers) {
        worker = std::async(std::launch::async, [&] {
            return std::pair{
                hash_blake3_file(path),
                compute_pdq(image.luma, image.width, image.height)
            };
        });
    }

    for (auto &worker : workers) {
        EXPECT_EQ(worker.get(), expected);
    }
}

} // namespace
