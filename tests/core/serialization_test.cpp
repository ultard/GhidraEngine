#include <GhidraEngine/hash/blake3.hpp>
#include <GhidraEngine/io/fingerprint.hpp>

#include "../support/test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <limits>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

void sign(std::vector<std::byte> &bytes) {
    const auto digest = hash_blake3(std::span(bytes).first(bytes.size() - 32));

    for (unsigned i = 0; i < 32; ++i) {
        bytes[bytes.size() - 32 + i] = static_cast<std::byte>(digest.bytes[i]);
    }
}

TEST(FingerprintIo, ImageWireBytesHaveFrozenHeaderWordOrderAndQuality) {
    const ImageSignature image{
        {{PdqHash{{0x0807060504030201ULL, 0, 0, 0x8000000000000000ULL}}, PdqQuality{100}}}
    };
    // Version-1 specification: 15-byte header, four little-endian words, quality, checksum.
    const std::array<unsigned char, 48> payload{
        'G',
        'H',
        'F',
        'P',
        1,
        0,
        1,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        128,
        100
    };
    std::vector<std::byte> expected;

    for (auto byte : payload) {
        expected.push_back(static_cast<std::byte>(byte));
    }

    const auto checksum = hash_blake3(expected);

    for (auto byte : checksum.bytes) {
        expected.push_back(static_cast<std::byte>(byte));
    }

    EXPECT_EQ(serialize_fingerprint(image), expected);
    EXPECT_EQ(deserialize_fingerprint(expected), MediaFingerprint{image});
}

TEST(FingerprintIo, VideoWirePreservesSignedTimestampExtremesAndSuppliedOrder) {
    const VpdqSignature video{
        {{prefix_hash(1), PdqQuality{0}, Timestamp{std::numeric_limits<std::int64_t>::min()}},
         {prefix_hash(256), PdqQuality{100}, Timestamp{std::numeric_limits<std::int64_t>::max()}},
         {{}, PdqQuality{50}, Timestamp{-1}}}
    };
    const auto bytes = serialize_fingerprint(video);

    ASSERT_EQ(bytes.size(), 47U + 3 * 41);
    EXPECT_EQ(bytes[6], std::byte{2});
    EXPECT_EQ(bytes[7], std::byte{3});

    for (unsigned i = 0; i < 7; ++i) {
        EXPECT_EQ(bytes[48 + i], std::byte{0});
    }

    EXPECT_EQ(bytes[55], std::byte{128});
    EXPECT_EQ(deserialize_fingerprint(bytes), MediaFingerprint{video});
    EXPECT_THROW((void)deserialize_fingerprint(bytes, 2), std::invalid_argument);
    EXPECT_EQ(deserialize_fingerprint(bytes, 3), MediaFingerprint{video});
    EXPECT_EQ(
        deserialize_fingerprint(serialize_fingerprint(VpdqSignature{})),
        MediaFingerprint{VpdqSignature{}}
    );
    EXPECT_THROW((void)serialize_fingerprint(ImageSignature{}), std::invalid_argument);
    EXPECT_THROW(
        (void)serialize_fingerprint(ImageSignature{std::vector<PdqFingerprint>(9)}),
        std::invalid_argument
    );
}

TEST(FingerprintIo, EveryTruncationAndSingleByteCorruptionIsRejected) {
    const auto valid = serialize_fingerprint(ImageSignature{{{prefix_hash(256), PdqQuality{100}}}});

    for (std::size_t i = 0; i < valid.size(); ++i) {
        SCOPED_TRACE(i);

        EXPECT_THROW(
            (void)deserialize_fingerprint(std::span(valid).first(i)),
            std::invalid_argument
        );

        auto corrupt = valid;
        corrupt[i] ^= std::byte{1};

        EXPECT_THROW((void)deserialize_fingerprint(corrupt), std::invalid_argument);
    }
}

TEST(FingerprintIo, ValidChecksumDoesNotBypassFieldLengthOrBudgetValidation) {
    const auto valid = serialize_fingerprint(ImageSignature{{{{}, PdqQuality{100}}}});

    for (unsigned offset : {0U, 4U, 5U, 6U, 7U, 47U}) {
        SCOPED_TRACE(offset);
        auto malformed = valid;
        malformed[offset] = std::byte{255};
        sign(malformed);

        EXPECT_THROW((void)deserialize_fingerprint(malformed), std::invalid_argument);
    }

    auto zero_count = valid;
    zero_count[7] = std::byte{0};
    sign(zero_count);

    EXPECT_THROW((void)deserialize_fingerprint(zero_count), std::invalid_argument);

    auto extra = valid;
    extra.push_back(std::byte{0});
    sign(extra);

    EXPECT_THROW((void)deserialize_fingerprint(extra), std::invalid_argument);

    auto huge = serialize_fingerprint(VpdqSignature{});

    for (unsigned i = 7; i < 15; ++i) {
        huge[i] = std::byte{255};
    }

    sign(huge);

    EXPECT_THROW((void)deserialize_fingerprint(huge), std::invalid_argument);
}

TEST(FingerprintIo, SeededRoundtripPreservesEveryFrameBit) {
    std::mt19937_64 random(42);

    for (unsigned count : {0U, 1U, 2U, 8U, 64U, 257U}) {
        VpdqSignature video;

        for (unsigned i = 0; i < count; ++i) {
            video.frames.push_back(
                {random_hash(random),
                 PdqQuality{static_cast<int>(random() % 101)},
                 Timestamp{std::bit_cast<std::int64_t>(random())}}
            );
        }

        EXPECT_EQ(deserialize_fingerprint(serialize_fingerprint(video)), MediaFingerprint{video});
    }
}

} // namespace
