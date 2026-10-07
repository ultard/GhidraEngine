#include <GhidraEngine/index/mih_pdq.hpp>
#include <GhidraEngine/match/image.hpp>

#include "../support/test_support.hpp"

#include <gtest/gtest.h>
#include <pdq/cpp/hashing/pdqhashing.h>

#include <array>
#include <future>
#include <limits>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

std::vector<PdqFingerprint> upstream_variants(const DecodedImage &image) {
    namespace pdq = facebook::pdq::hashing;
    auto input = image.luma;
    std::vector<float> scratch(input.size());
    float b64[64][64], b16x64[16][64], b16[16][16], auxiliary[16][16];
    std::array<pdq::Hash256, 8> hashes;
    int quality = 0;

    if (!pdq::pdqDihedralHash256esFromFloatLuma(
            input.data(),
            scratch.data(),
            static_cast<int>(image.height),
            static_cast<int>(image.width),
            b64,
            b16x64,
            b16,
            auxiliary,
            &hashes[0],
            &hashes[1],
            &hashes[2],
            &hashes[3],
            &hashes[4],
            &hashes[5],
            &hashes[6],
            &hashes[7],
            quality
        )) {
        throw std::runtime_error("Upstream dihedral computation failed");
    }

    std::vector<PdqFingerprint> result(8);

    for (unsigned variant = 0; variant < 8; ++variant) {
        result[variant].quality = PdqQuality{quality};

        for (unsigned bit = 0; bit < 256; ++bit) {
            if (hashes[variant].getBit(static_cast<int>(bit))) {
                result[variant].hash.words[bit / 64] |= std::uint64_t{1} << (bit % 64);
            }
        }
    }

    return result;
}

// Pixel transforms independently describe the order of the public variant array.
DecodedImage transform(const DecodedImage &image, unsigned orientation) {
    const auto w = image.width, h = image.height;
    const bool transpose = orientation == 1 || orientation == 3 || orientation >= 6;
    DecodedImage result{transpose ? h : w, transpose ? w : h, std::vector<float>(w * h)};

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::array positions{
                y * w + x,
                (w - 1 - x) * h + y,
                (h - 1 - y) * w + w - 1 - x,
                x * h + h - 1 - y,
                (h - 1 - y) * w + x,
                y * w + w - 1 - x,
                x * h + y,
                (w - 1 - x) * h + h - 1 - y
            };
            result.luma[positions[orientation]] = image.luma[y * w + x];
        }
    }

    return result;
}

TEST(ImageFingerprint, PoliciesPreserveOrderedPrefixesAndMatchUpstream) {
    for (auto [w, h] : {std::pair{1U, 1U}, {4U, 100U}, {7U, 9U}, {64U, 64U}, {193U, 65U}}) {
        const auto image = pattern_image(w, h);
        const auto before = image.luma;
        const auto original = fingerprint_image(image);
        const auto rotations = fingerprint_image(image, TransformPolicy::Rotations);
        const auto dihedral = fingerprint_image(image, TransformPolicy::Dihedral);

        ASSERT_EQ(original.variants.size(), 1U);
        ASSERT_EQ(rotations.variants.size(), 4U);
        ASSERT_EQ(dihedral.variants.size(), 8U);
        EXPECT_EQ(original.variants[0], compute_pdq(image.luma, w, h));
        EXPECT_EQ(original.variants[0], dihedral.variants[0]);
        EXPECT_TRUE(
            std::equal(
                rotations.variants.begin(),
                rotations.variants.end(),
                dihedral.variants.begin()
            )
        );

        if (w >= 5 && h >= 5) {
            EXPECT_EQ(dihedral.variants, upstream_variants(image));
        }

        EXPECT_EQ(image.luma, before);
    }

    const auto photo = decode_image(fixture("image/bridge.png"));

    EXPECT_EQ(
        fingerprint_image(photo, TransformPolicy::Dihedral).variants,
        upstream_variants(photo)
    );
}

TEST(ImageFingerprint, PhysicalTransformsControlMatchesWithBothIndexes) {
    auto image = pattern_image(64, 64);
    std::mt19937_64 random(0x696d616765);

    for (auto &pixel : image.luma) {
        pixel = static_cast<float>(random() % 256);
    }

    const auto original = fingerprint_image(image).variants[0];
    const auto dihedral = fingerprint_image(image, TransformPolicy::Dihedral);
    const std::array records{ImageFingerprintRecord{FingerprintId{0}, MediaId{1}, original}};
    const ImageFingerprintCatalog catalog(records);
    const std::array entries{PdqIndexEntry{FingerprintId{0}, original.hash}};
    const FlatPdqIndex flat(entries);
    const MihPdqIndex mih(entries);

    for (unsigned orientation = 0; orientation < 8; ++orientation) {
        SCOPED_TRACE(orientation);
        const auto pixels = transform(image, orientation);

        EXPECT_EQ(
            compute_pdq(pixels.luma, pixels.width, pixels.height),
            dihedral.variants[orientation]
        );

        for (auto policy :
             {TransformPolicy::OriginalOnly,
              TransformPolicy::Rotations,
              TransformPolicy::Dihedral}) {
            const auto query = fingerprint_image(pixels, policy);
            const auto ids = find_image_candidates(flat, query, PdqDistance{0});

            EXPECT_EQ(find_image_candidates(mih, query, PdqDistance{0}), ids);

            const auto matches = verify_image_candidates(
                MediaId{0},
                query,
                ids,
                catalog,
                {PdqDistance{0}, PdqQuality{0}}
            );
            const bool allowed = orientation == 0 || policy == TransformPolicy::Dihedral ||
                                 (orientation < 4 && policy == TransformPolicy::Rotations);

            EXPECT_EQ(matches.size(), allowed ? 1U : 0U);
        }
    }
}

TEST(ImageMatch, InclusivePolicyDeduplicatesMediaAndSelectsSmallestEligibleDistance) {
    const ImageSignature query{{{{}, PdqQuality{49}}, {prefix_hash(1), PdqQuality{50}}}};
    const std::array records{
        ImageFingerprintRecord{FingerprintId{9}, MediaId{2}, {prefix_hash(4), PdqQuality{80}}},
        ImageFingerprintRecord{FingerprintId{1}, MediaId{2}, {{}, PdqQuality{50}}},
        ImageFingerprintRecord{FingerprintId{3}, MediaId{1}, {prefix_hash(3), PdqQuality{100}}},
        ImageFingerprintRecord{FingerprintId{4}, MediaId{2}, {prefix_hash(1), PdqQuality{49}}},
        ImageFingerprintRecord{FingerprintId{0}, MediaId{0}, {prefix_hash(1), PdqQuality{100}}}
    };
    const ImageFingerprintCatalog catalog(records);
    const std::array ids{
        FingerprintId{9},
        FingerprintId{3},
        FingerprintId{1},
        FingerprintId{4},
        FingerprintId{0},
        FingerprintId{9}
    };

    EXPECT_EQ(
        verify_image_candidates(MediaId{0}, query, ids, catalog, {PdqDistance{3}, PdqQuality{50}}),
        (std::vector<ImageMatch>{
            {MediaId{0}, MediaId{1}, PdqDistance{2}},
            {MediaId{0}, MediaId{2}, PdqDistance{1}}
        })
    );
    EXPECT_TRUE(
        verify_image_candidates(MediaId{0}, query, ids, catalog, {PdqDistance{0}, PdqQuality{50}})
            .empty()
    );
}

TEST(ImageMatch, CatalogOwnsSortedDataAndVerificationRejectsStaleHashes) {
    std::array records{
        ImageFingerprintRecord{FingerprintId{7}, MediaId{1}, {prefix_hash(1), PdqQuality{100}}},
        ImageFingerprintRecord{FingerprintId{0}, MediaId{0}, {{}, PdqQuality{100}}}
    };
    const ImageFingerprintCatalog catalog(records);
    records[0].fingerprint.hash = {};

    EXPECT_EQ(catalog.records().front().id, FingerprintId{0});
    EXPECT_EQ(catalog.at(FingerprintId{7}).fingerprint.hash, prefix_hash(1));
    EXPECT_THROW((void)catalog.at(FingerprintId{99}), std::out_of_range);

    records[0].id = records[1].id;

    EXPECT_THROW((void)ImageFingerprintCatalog{records}, std::invalid_argument);

    FlatPdqIndex stale;
    stale.insert(FingerprintId{7}, {});
    const ImageSignature query{{{{}, PdqQuality{100}}}};
    const auto ids = find_image_candidates(stale, query, PdqDistance{0});

    EXPECT_TRUE(
        verify_image_candidates(MediaId{0}, query, ids, catalog, {PdqDistance{0}, PdqQuality{0}})
            .empty()
    );
    EXPECT_THROW((void)verify_image_candidates(MediaId{0}, query, ids, {}, {}), std::out_of_range);
}

TEST(ImageMatch, InvalidQueriesPoliciesAndPixelsAreRejected) {
    for (const auto &query : {ImageSignature{}, ImageSignature{std::vector<PdqFingerprint>(9)}}) {
        EXPECT_THROW((void)find_image_candidates(FlatPdqIndex{}, query, {}), std::invalid_argument);
        EXPECT_THROW(
            (void)verify_image_candidates(MediaId{}, query, {}, {}, {}),
            std::invalid_argument
        );
    }

    EXPECT_THROW(
        (void)fingerprint_image(pattern_image(5, 5), static_cast<TransformPolicy>(255)),
        std::invalid_argument
    );
    EXPECT_THROW((void)fingerprint_image(DecodedImage{}), std::invalid_argument);

    for (float bad :
         {-1.0F,
          256.0F,
          std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_THROW(
            (void)fingerprint_image(DecodedImage{1, 1, {bad}}, TransformPolicy::Dihedral),
            std::invalid_argument
        );
    }
}

TEST(ImageMatch, RetrievalQueriesAllVariantsWithoutQualityOrMediaFiltering) {
    struct RecordingIndex {
        mutable std::vector<PdqHash> queries;

        void insert(FingerprintId, const PdqHash &) {
        }

        std::vector<PdqHit> search_within(const PdqHash &hash, std::uint16_t radius) const {
            queries.push_back(hash);

            EXPECT_EQ(radius, 31);

            return {{FingerprintId{0}, PdqDistance{1}}, {FingerprintId{9}, PdqDistance{0}}};
        }
    } index;

    const ImageSignature query{{{{}, PdqQuality{0}}, {prefix_hash(1), PdqQuality{100}}}};

    EXPECT_EQ(
        find_image_candidates(index, query, PdqDistance{31}),
        (std::vector<FingerprintId>{{0}, {9}})
    );
    EXPECT_EQ(index.queries, (std::vector<PdqHash>{{}, prefix_hash(1)}));
}

TEST(ImageMatch, FullRadiusRepeatedVariantsNeverCapOrDuplicateMedia) {
    std::vector<ImageFingerprintRecord> records(10000);
    std::vector<PdqIndexEntry> entries;

    for (std::size_t i = 0; i < records.size(); ++i) {
        records[i] = {FingerprintId{i}, MediaId{i}, {prefix_hash(256), PdqQuality{0}}};
        entries.push_back({FingerprintId{i}, prefix_hash(256)});
    }

    const ImageFingerprintCatalog catalog(records);
    const FlatPdqIndex index(entries);
    const ImageSignature query{std::vector<PdqFingerprint>(8)};
    const auto ids = find_image_candidates(index, query, PdqDistance{256});

    ASSERT_EQ(ids.size(), records.size());

    const auto matches = verify_image_candidates(
        MediaId{std::numeric_limits<std::uint64_t>::max()},
        query,
        ids,
        catalog,
        {PdqDistance{256}, PdqQuality{0}}
    );

    ASSERT_EQ(matches.size(), records.size());

    for (std::size_t i = 0; i < matches.size(); ++i) {
        EXPECT_EQ(matches[i].candidate, MediaId{i});
        EXPECT_EQ(matches[i].distance, PdqDistance{256});
    }

    EXPECT_TRUE(verify_image_candidates(
                    MediaId{10001},
                    query,
                    ids,
                    catalog,
                    {PdqDistance{255}, PdqQuality{0}}
    )
                    .empty());
}

TEST(ImageMatch, RealPhotoPipelineUsesMihAndConcurrentScratchIsIndependent) {
    const auto image = decode_image(fixture("image/bridge.png"));
    const auto signature = fingerprint_image(image, TransformPolicy::Dihedral);
    std::mt19937_64 random(0x70697065);
    std::vector<ImageFingerprintRecord> records;
    std::vector<PdqIndexEntry> entries;

    for (unsigned i = 0; i < 50000; ++i) {
        const auto hash = random_hash(random);
        records.push_back({FingerprintId{100 + i}, MediaId{100 + i}, {hash, PdqQuality{100}}});
        entries.push_back({FingerprintId{100 + i}, hash});
    }

    for (std::size_t i = 0; i < signature.variants.size(); ++i) {
        records.push_back({FingerprintId{i}, MediaId{1}, signature.variants[i]});
        entries.push_back({FingerprintId{i}, signature.variants[i].hash});
    }

    const ImageFingerprintCatalog catalog(records);
    const FlatPdqIndex flat(entries);
    const MihPdqIndex mih(entries);
    MihSearchStats stats;

    EXPECT_EQ(
        mih.search_within(signature.variants[0].hash, 31, &stats),
        flat.search_within(signature.variants[0].hash, 31)
    );
    EXPECT_FALSE(stats.used_flat);

    std::array<std::future<std::vector<ImageMatch>>, 4> workers;

    for (auto &worker : workers) {
        worker = std::async(std::launch::async, [&] {
            const auto query = fingerprint_image(image, TransformPolicy::Dihedral);
            const auto candidates = find_image_candidates(mih, query, PdqDistance{31});

            return verify_image_candidates(
                MediaId{0},
                query,
                candidates,
                catalog,
                {PdqDistance{31}, PdqQuality{50}}
            );
        });
    }

    for (auto &worker : workers) {
        EXPECT_EQ(
            worker.get(),
            (std::vector<ImageMatch>{{MediaId{0}, MediaId{1}, PdqDistance{0}}})
        );
    }
}

} // namespace
