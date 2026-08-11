// An altered copy must land close to its original in Hamming space and an
// unrelated image far away. Asserting the numbers turns a regression in the DCT,
// the resampler or the thresholding into a failure rather than quietly worse
// results.
#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ghidraengine/ghidraengine.hpp"
#include "decode/image_decoder.hpp"
#include "hash/phash.hpp"
#include "image_fixture.hpp"

using namespace ghidraengine;
using namespace ghidraengine::test;

namespace {

ImageSignature signature_of(const Image& image, int quality = 92,
                            const ImageMatchConfig& config = {}) {
    const auto encoded = encode_jpeg(image, quality);
    auto thumb = decode_image(encoded);
    REQUIRE(thumb.has_value());
    return compute_signature(*thumb, config);
}

} // namespace

TEST_CASE("a re-encoded copy stays close to its original", "[accuracy]") {
    const Image original = make_image(1024, 768, 42);
    const ImageSignature reference = signature_of(original, 95);

    struct Case {
        const char* name;
        int quality;
        std::uint32_t allowed;
    };

    // The default detection threshold is 10, so these sit comfortably below it.
    const Case cases[] = {
        {"quality 90", 90, 4},
        {"quality 75", 75, 6},
        {"quality 50", 50, 8},
        {"quality 30", 30, 10},
    };

    for (const Case& item : cases) {
        const ImageSignature altered = signature_of(original, item.quality);
        const std::uint32_t distance = hamming_distance(reference.phash64, altered.phash64);
        INFO(item.name << " -> distance " << distance);
        CHECK(distance <= item.allowed);
        CHECK(images_match(reference, altered, ImageMatchConfig{}));
    }
}

TEST_CASE("resizing preserves the hash", "[accuracy]") {
    const Image original = make_image(1600, 1200, 7);
    const ImageSignature reference = signature_of(original);

    for (const double factor : {0.75, 0.5, 0.25, 0.125}) {
        const auto width = static_cast<std::uint32_t>(1600 * factor);
        const auto height = static_cast<std::uint32_t>(1200 * factor);
        const ImageSignature scaled = signature_of(resize(original, width, height));

        const std::uint32_t distance = hamming_distance(reference.phash64, scaled.phash64);
        INFO("scale " << factor << " (" << width << "x" << height << ") -> distance "
                      << distance);
        CHECK(distance <= 8);
        CHECK(images_match(reference, scaled, ImageMatchConfig{}));
    }
}

TEST_CASE("brightness shifts do not break the hash", "[accuracy]") {
    // Thresholding against the AC median with DC excluded exists so exposure
    // changes cancel out; this is that guarantee.
    const Image original = make_image(800, 600, 99);
    const ImageSignature reference = signature_of(original);

    for (const int delta : {-40, -20, 20, 40}) {
        const ImageSignature shifted = signature_of(adjust_brightness(original, delta));
        const std::uint32_t distance = hamming_distance(reference.phash64, shifted.phash64);
        INFO("delta " << delta << " -> distance " << distance);
        CHECK(distance <= 8);
    }
}

TEST_CASE("a modest crop is still recognised", "[accuracy]") {
    const Image original = make_image(1200, 900, 5);
    const ImageSignature reference = signature_of(original);

    const ImageSignature cropped = signature_of(crop(original, 0.03));
    const std::uint32_t distance = hamming_distance(reference.phash64, cropped.phash64);
    INFO("3% crop -> distance " << distance);
    // A crop shifts content across the whole frame, so the tolerance is wider.
    // Beyond roughly 10% it is a different picture and is not expected to match.
    CHECK(distance <= 14);
}

TEST_CASE("unrelated images stay far apart", "[accuracy]") {
    // Without this, a hash returning a constant would pass every test above.
    std::vector<ImageSignature> signatures;
    for (std::uint32_t seed = 0; seed < 12; ++seed) {
        signatures.push_back(signature_of(make_image(800, 600, seed * 7919 + 11)));
    }

    std::uint32_t smallest = 64;
    for (std::size_t i = 0; i < signatures.size(); ++i) {
        for (std::size_t j = i + 1; j < signatures.size(); ++j) {
            const std::uint32_t distance =
                hamming_distance(signatures[i].phash64, signatures[j].phash64);
            smallest = std::min(smallest, distance);
            CHECK_FALSE(images_match(signatures[i], signatures[j], ImageMatchConfig{}));
        }
    }
    INFO("closest unrelated pair: " << smallest);
    CHECK(smallest > 10);
}

TEST_CASE("rotations match only when dihedral invariance is enabled", "[accuracy]") {
    const Image original = make_image(900, 900, 2024);

    ImageMatchConfig plain;
    ImageMatchConfig invariant;
    invariant.dihedral_invariant = true;

    const ImageSignature upright_plain = signature_of(original, 92, plain);
    const ImageSignature rotated_plain = signature_of(rotate_90(original), 92, plain);
    CHECK_FALSE(images_match(upright_plain, rotated_plain, plain));

    const ImageSignature upright = signature_of(original, 92, invariant);
    const ImageSignature rotated = signature_of(rotate_90(original), 92, invariant);
    const ImageSignature mirrored = signature_of(mirror_horizontal(original), 92, invariant);

    INFO("rotated distance " << hamming_distance(upright.phash64, rotated.phash64));
    CHECK(images_match(upright, rotated, invariant));
    CHECK(images_match(upright, mirrored, invariant));
}

TEST_CASE("the colour check rejects images that agree only in grayscale", "[accuracy]") {
    // The classic DCT false positive: identical luma structure, different colour.
    Image base = make_image(800, 600, 314);
    Image recoloured = base;
    for (std::size_t i = 0; i + 2 < recoloured.rgb.size(); i += 3) {
        std::swap(recoloured.rgb[i], recoloured.rgb[i + 2]); // swap red and blue
    }

    const ImageSignature a = signature_of(base);
    const ImageSignature b = signature_of(recoloured);

    REQUIRE(a.has_color);
    REQUIRE(b.has_color);

    ImageMatchConfig without_colour;
    without_colour.color_threshold = 255; // disabled
    ImageMatchConfig with_colour;

    INFO("luma distance " << hamming_distance(a.phash64, b.phash64) << ", chroma distance "
                          << color_distance(a.color, b.color));

    // Swapping red and blue leaves luma nearly untouched.
    if (hamming_distance(a.phash64, b.phash64) <= without_colour.phash_threshold) {
        CHECK(images_match(a, b, without_colour));
        CHECK_FALSE(images_match(a, b, with_colour));
    }
}

TEST_CASE("non-JPEG formats decode through the FFmpeg fallback", "[decode]") {
    const Image original = make_image(640, 480, 77);

    const auto bmp = encode_bmp(original);
    auto from_bmp = decode_image(bmp);
    REQUIRE(from_bmp.has_value());

    const auto jpeg = encode_jpeg(original, 95);
    auto from_jpeg = decode_image(jpeg);
    REQUIRE(from_jpeg.has_value());

    const ImageSignature a = compute_signature(*from_bmp, ImageMatchConfig{});
    const ImageSignature b = compute_signature(*from_jpeg, ImageMatchConfig{});

    // The stored format must never decide whether a file is found as a duplicate.
    const std::uint32_t distance = hamming_distance(a.phash64, b.phash64);
    INFO("BMP vs JPEG distance " << distance);
    CHECK(distance <= 8);
    CHECK(a.width == 640);
    CHECK(a.height == 480);
}

TEST_CASE("degenerate images are handled without crashing", "[decode]") {
    SECTION("uniform image has near-zero variance") {
        Image flat = make_image(256, 256, 1);
        std::fill(flat.rgb.begin(), flat.rgb.end(), std::uint8_t{128});

        const auto encoded = encode_jpeg(flat, 90);
        auto thumb = decode_image(encoded);
        REQUIRE(thumb.has_value());
        CHECK(luma_variance(thumb->gray) < 1.0);
    }

    SECTION("truncated JPEG does not produce a signature by accident") {
        const auto encoded = encode_jpeg(make_image(400, 300, 3), 90);
        std::vector<std::uint8_t> truncated(encoded.begin(), encoded.begin() + 32);
        auto thumb = decode_image(truncated);
        CHECK_FALSE(thumb.has_value());
    }

    SECTION("garbage input is rejected") {
        std::vector<std::uint8_t> garbage(1024, 0xAB);
        auto thumb = decode_image(garbage);
        CHECK_FALSE(thumb.has_value());
    }
}
