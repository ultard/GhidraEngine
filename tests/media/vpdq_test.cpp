#include <GhidraEngine/hash/pdq.hpp>
#include <GhidraEngine/index/mih_pdq.hpp>
#include <GhidraEngine/index/video.hpp>

#include "../../src/match/vpdq_byte_index.hpp"
#include "../support/test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <future>
#include <set>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

// Preserve first occurrence before quality filtering, independently of production sorting.
std::vector<PdqHash> eligible(const VpdqSignature &signature, unsigned quality) {
    std::set<PdqHash> seen;
    std::vector<PdqHash> hashes;

    for (const auto &frame : signature.frames) {
        if (seen.insert(frame.hash).second && frame.quality.value() >= quality) {
            hashes.push_back(frame.hash);
        }
    }

    return hashes;
}

VpdqComparison
oracle(const VpdqSignature &query, const VpdqSignature &candidate, const PdqMatchPolicy &policy) {
    const auto q = eligible(query, policy.min_quality.value());
    const auto c = eligible(candidate, policy.min_quality.value());
    std::vector<bool> matched_q(q.size()), matched_c(c.size());

    for (std::size_t i = 0; i < q.size(); ++i) {
        for (std::size_t j = 0; j < c.size(); ++j) {
            int distance = 0;

            for (unsigned word = 0; word < 4; ++word) {
                distance += std::popcount(q[i].words[word] ^ c[j].words[word]);
            }

            if (distance <= policy.max_distance.value()) {
                matched_q[i] = matched_c[j] = true;
            }
        }
    }

    VpdqComparison result{{}, {}, q.size(), c.size()};

    if (!q.empty() && !c.empty()) {
        result.query_coverage =
            Coverage{static_cast<double>(std::ranges::count(matched_q, true)) /
                     static_cast<double>(q.size())};
        result.candidate_coverage =
            Coverage{static_cast<double>(std::ranges::count(matched_c, true)) /
                     static_cast<double>(c.size())};
    }

    return result;
}

std::vector<VideoMatch> oracle_matches(
    VideoId query_id,
    const VpdqSignature &query,
    const VideoFingerprintCatalog &catalog,
    const VpdqMatchPolicy &policy
) {
    std::vector<VideoMatch> matches;

    for (const auto &record : catalog.records()) {
        const auto comparison =
            oracle(query, record.signature, {policy.max_distance, policy.min_quality});
        if (record.id != query_id && comparison.query_frames && comparison.candidate_frames &&
            comparison.query_coverage.value() >= policy.min_query_coverage.value() &&
            comparison.candidate_coverage.value() >= policy.min_candidate_coverage.value()) {
            matches.push_back(
                {query_id, record.id, comparison.query_coverage, comparison.candidate_coverage}
            );
        }
    }

    return matches;
}

TEST(Vpdq, FirstDuplicateWinsBeforeQualityFilterAndCoverageIsDirectional) {
    const VpdqSignature query{
        {{{}, PdqQuality{49}, Timestamp{-1}},
         {{}, PdqQuality{100}, {}},
         {prefix_hash(1), PdqQuality{50}, Timestamp{1}},
         {prefix_hash(2), PdqQuality{100}, Timestamp{2}}}
    };
    const VpdqSignature candidate{{{{}, PdqQuality{50}, Timestamp{1000000}}}};
    const auto exact = compare_vpdq(query, candidate, {PdqDistance{0}, PdqQuality{50}});

    EXPECT_EQ(exact.query_frames, 2U);
    EXPECT_EQ(exact.query_coverage, Coverage{0});

    const auto inclusive = compare_vpdq(query, candidate, {PdqDistance{1}, PdqQuality{50}});

    EXPECT_EQ(inclusive.query_coverage, Coverage{0.5});
    EXPECT_EQ(inclusive.candidate_coverage, Coverage{1});
    EXPECT_EQ(inclusive, oracle(query, candidate, {PdqDistance{1}, PdqQuality{50}}));

    const auto reversed = compare_vpdq(candidate, query, {PdqDistance{1}, PdqQuality{50}});

    EXPECT_EQ(reversed.query_coverage, inclusive.candidate_coverage);
    EXPECT_EQ(reversed.candidate_coverage, inclusive.query_coverage);
    EXPECT_EQ(
        compare_vpdq(query, candidate, {PdqDistance{2}, PdqQuality{50}}).query_coverage,
        Coverage{1}
    );
}

TEST(Vpdq, EmptyEligibleSidesAndSelfNeverMatchEvenWithZeroCoverageMinima) {
    const VpdqSignature high{{{{}, PdqQuality{50}, {}}}}, low{{{{}, PdqQuality{49}, {}}}};
    const VpdqMatchPolicy policy{PdqDistance{256}, PdqQuality{50}, Coverage{0}, Coverage{0}};

    for (const auto &empty : {VpdqSignature{}, low}) {
        const auto result = compare_vpdq(empty, high, {policy.max_distance, policy.min_quality});

        EXPECT_EQ(result.query_coverage, Coverage{0});
        EXPECT_EQ(result.candidate_coverage, Coverage{0});
        EXPECT_FALSE(match_vpdq(VideoId{0}, empty, VideoId{1}, high, policy));
        EXPECT_FALSE(match_vpdq(VideoId{0}, high, VideoId{1}, empty, policy));
    }

    EXPECT_FALSE(match_vpdq(VideoId{0}, high, VideoId{0}, high, policy));
}

TEST(Vpdq, SubclipCoverageMinimaAreIndependentAndInclusive) {
    const VpdqSignature full{
        {{{}, PdqQuality{100}, {}}, {prefix_hash(256), PdqQuality{100}, Timestamp{1}}}
    };
    const VpdqSignature clip{{full.frames[0]}};
    VpdqMatchPolicy policy{PdqDistance{0}, PdqQuality{0}, Coverage{1}, Coverage{0.5}};
    const auto match = match_vpdq(VideoId{0}, clip, VideoId{1}, full, policy);

    ASSERT_TRUE(match);
    EXPECT_EQ(match->query_coverage, Coverage{1});
    EXPECT_EQ(match->candidate_coverage, Coverage{0.5});

    policy.min_candidate_coverage = Coverage{0.500001};

    EXPECT_FALSE(match_vpdq(VideoId{0}, clip, VideoId{1}, full, policy));
}

TEST(Vpdq, SparseBytePartitionBoundariesAgreeWithIndependentExhaustiveCoverage) {
    std::mt19937_64 random(0x6279746573);
    VpdqSignature query, candidate;

    for (unsigned i = 0; i < 640; ++i) {
        const auto hash = random_hash(random);
        auto changed = hash;
        // The one unchanged byte rotates through all 32 partitions.
        for (unsigned slot = 0; slot < 32; ++slot) {
            if (slot != i % 32) {
                changed.words[slot / 8] ^= std::uint64_t{1} << (8 * (slot % 8));
            }
        }

        query.frames.push_back({hash, PdqQuality{i % 17 == 0 ? 49 : 100}, Timestamp{i}});
        candidate.frames.push_back({changed, PdqQuality{100}, Timestamp{-std::int64_t{i}}});
    }

    for (std::size_t count : {511U, 512U, 513U, 640U}) {
        auto q = query, c = candidate;
        q.frames.resize(count);
        c.frames.resize(count);
        q.frames.push_back({q.frames[0].hash, PdqQuality{100}, {}});

        for (int radius : {0, 30, 31, 32, 47, 63, 64, 95, 127, 255, 256}) {
            SCOPED_TRACE(count);
            SCOPED_TRACE(radius);
            const PdqMatchPolicy policy{PdqDistance{radius}, PdqQuality{0}};

            EXPECT_EQ(compare_vpdq(q, c, policy), oracle(q, c, policy));
        }
    }

    EXPECT_EQ(
        compare_vpdq(query, candidate, {PdqDistance{31}, PdqQuality{50}}),
        oracle(query, candidate, {PdqDistance{31}, PdqQuality{50}})
    );
}

TEST(Vpdq, MixedByteRadiiAndChunkedIndexesKeepInclusiveBoundaryMatches) {
    std::mt19937_64 random(12345);

    for (unsigned radius : {0U, 31U, 32U, 63U, 64U, 95U, 127U, 255U}) {
        std::vector<PdqHash> hashes(65536);

        for (auto &hash : hashes) {
            hash = random_hash(random);
        }

        detail::VpdqByteIndex index(hashes, static_cast<std::uint16_t>(radius));
        index.build_positions(hashes);

        for (unsigned slot = 0; slot < 32; ++slot) {
            auto query = hashes[slot];

            for (unsigned part = 0; part < 32; ++part) {
                const auto bits = radius / 32 + (part < radius % 32 ? 1U : 0U);
                const auto rotated = (part + slot) % 32;

                for (unsigned bit = 0; bit < bits; ++bit) {
                    const auto position = rotated * 8 + (bit + slot) % 8;
                    query.words[position / 64] ^= std::uint64_t{1} << (position % 64);
                }
            }

            EXPECT_TRUE(index.contains_match(query, hashes, static_cast<std::uint16_t>(radius)));
        }
    }

    std::vector<PdqHash> from(512), to(65538);

    for (auto &hash : from) {
        hash = random_hash(random);
    }

    for (auto &hash : to) {
        hash = random_hash(random);
    }

    // One query matches in both chunks: count it once. Another matches only in the second.
    to[0] = to[65536] = from[0];
    to[65537] = from[1];

    const auto result = detail::vpdq_coverage_count(from, to, 0);

    EXPECT_EQ(result.matches, 2U);
    EXPECT_TRUE(result.used_index);
}

TEST(VideoCandidates, CommonFrameAndLaterMissesReturnSortedUniqueVideos) {
    std::vector<VideoSignatureRecord> records;

    for (unsigned i = 0; i < 100; ++i) {
        records.push_back({VideoId{100 - i}, VpdqSignature{{{{}, PdqQuality{100}, {}}}}});
    }

    const VideoFingerprintCatalog catalog(records);
    const MihPdqIndex index(catalog.index_entries());
    VpdqSignature query{{{{}, PdqQuality{100}, {}}}};

    for (unsigned i = 1; i <= 100; ++i) {
        query.frames.push_back({prefix_hash(i), PdqQuality{100}, Timestamp{i}});
    }

    VpdqMatchPolicy policy;
    policy.max_distance = PdqDistance{0};
    const auto candidates = find_video_candidates(index, catalog, query, policy);

    ASSERT_EQ(candidates.size(), 100U);

    for (unsigned i = 0; i < candidates.size(); ++i) {
        EXPECT_EQ(candidates[i], VideoId{i + 1});
    }
}

TEST(Vpdq, DenseOversizedAndSharedByteNegativeInputsPreserveCoverage) {
    VpdqSignature dense, query;

    for (unsigned i = 0; i < 65537; ++i) {
        dense.frames.push_back({PdqHash{{i, 0, 0, 0}}, PdqQuality{100}, {}});

        if (i < 512) {
            query.frames.push_back(dense.frames.back());
        }
    }

    const auto result = compare_vpdq(query, dense, {PdqDistance{31}, PdqQuality{50}});

    EXPECT_EQ(result.query_coverage, Coverage{1});
    EXPECT_EQ(result.candidate_coverage, Coverage{1});
    EXPECT_EQ(result.candidate_frames, 65537U);

    dense.frames.resize(640);

    EXPECT_EQ(
        compare_vpdq(query, dense, {PdqDistance{1}, PdqQuality{50}}),
        oracle(query, dense, {PdqDistance{1}, PdqQuality{50}})
    );

    std::mt19937_64 random(456);
    query.frames.clear();
    dense.frames.clear();

    for (unsigned i = 0; i < 768; ++i) {
        if (i < 512) {
            query.frames.push_back({PdqHash{{0, 0, random(), random()}}, PdqQuality{100}, {}});
        }

        dense.frames.push_back(
            {PdqHash{{~std::uint64_t{0}, 0, random(), random()}}, PdqQuality{100}, {}}
        );
    }

    EXPECT_EQ(
        compare_vpdq(query, dense, {PdqDistance{31}, PdqQuality{50}}),
        (VpdqComparison{Coverage{0}, Coverage{0}, 512, 768})
    );
}

TEST(Vpdq, FrozenPythonReferenceCorpus) {
    std::ifstream input(fixture("vpdq/reference.txt"));

    ASSERT_TRUE(input);

    std::size_t cases = 0;

    ASSERT_TRUE(input >> cases);
    ASSERT_EQ(cases, 300U);

    for (std::size_t i = 0; i < cases; ++i) {
        SCOPED_TRACE(i);
        std::size_t q = 0, c = 0;
        int radius = 0, quality = 0;
        double expected_q = 0, expected_c = 0;

        ASSERT_TRUE(input >> q >> c >> radius >> quality >> expected_q >> expected_c);

        std::array<VpdqSignature, 2> signatures;

        for (unsigned side = 0; side < 2; ++side) {
            for (std::size_t n = 0; n < (side == 0 ? q : c); ++n) {
                std::string hex;
                int frame_quality = 0;
                std::int64_t time = 0;

                ASSERT_TRUE(input >> hex >> frame_quality >> time);

                signatures[side].frames.push_back(
                    {decode_pdq(hex), PdqQuality{frame_quality}, Timestamp{time}}
                );
            }
        }

        const PdqMatchPolicy policy{PdqDistance{radius}, PdqQuality{quality}};
        const auto actual = compare_vpdq(signatures[0], signatures[1], policy);

        EXPECT_NEAR(actual.query_coverage.value(), expected_q, 1e-14);
        EXPECT_NEAR(actual.candidate_coverage.value(), expected_c, 1e-14);
        EXPECT_EQ(actual, oracle(signatures[0], signatures[1], policy));
    }

    std::string extra;

    EXPECT_FALSE(input >> extra);
}

TEST(VideoCatalog, OwnsSortedSignaturesAndDenseFrameMetadataAcrossCopyMove) {
    std::array records{
        VideoSignatureRecord{VideoId{9}, {{{prefix_hash(1), PdqQuality{80}, Timestamp{-10}}}}},
        VideoSignatureRecord{VideoId{0}, {}},
        VideoSignatureRecord{VideoId{3}, {{{{}, PdqQuality{50}, Timestamp{20}}}}}
    };
    const VideoFingerprintCatalog original(records);
    records[0].signature.frames.clear();

    EXPECT_EQ(original.records().front().id, VideoId{0});
    EXPECT_EQ(
        original.frame_at(FingerprintId{0}),
        (VideoFramePayload{FingerprintId{0}, VideoId{3}, Timestamp{20}, PdqQuality{50}})
    );
    EXPECT_EQ(original.at(VideoId{9}).frames.size(), 1U);
    EXPECT_THROW((void)original.at(VideoId{1}), std::out_of_range);
    EXPECT_THROW((void)original.frame_at(FingerprintId{2}), std::out_of_range);

    auto copy = original;
    const auto moved = VideoFingerprintCatalog(std::move(copy));

    EXPECT_TRUE(copy.records().empty());
    EXPECT_TRUE(copy.frames().empty());

    copy = moved;

    EXPECT_EQ(copy.index_entries(), original.index_entries());
    EXPECT_TRUE(std::ranges::equal(copy.frames(), original.frames()));

    records[0].id = records[1].id;

    EXPECT_THROW((void)VideoFingerprintCatalog{records}, std::invalid_argument);
}

TEST(VideoCatalog, BothIndexPipelinesHaveCompleteRecallAgainstIndependentMatcher) {
    std::mt19937_64 random(0x766964656f);
    std::vector<VideoSignatureRecord> records(128);

    for (std::size_t i = 0; i < records.size(); ++i) {
        records[i].id = VideoId{i};

        for (unsigned n = 0; n < 16; ++n) {
            records[i].signature.frames.push_back(
                {n == 0 ? prefix_hash(i % 8) : random_hash(random),
                 PdqQuality{static_cast<int>(random() % 101)},
                 Timestamp{n * 10000}}
            );
        }
    }

    records[0].signature.frames.push_back(records[0].signature.frames[0]);
    records.push_back({VideoId{999}, {}});
    const VideoFingerprintCatalog catalog(records);
    const auto entries = catalog.index_entries();
    const FlatPdqIndex flat(entries);
    const MihPdqIndex mih(entries);

    for (unsigned probe : {0U, 17U}) {
        for (int radius : {0, 1, 31, 128, 256}) {
            for (int quality : {0, 50}) {
                for (auto [qmin, cmin] :
                     {std::pair{0.0, 0.0}, {0.1, 0.0}, {0.0, 0.5}, {1.0, 1.0}}) {
                    const auto &record = records[probe];
                    const VpdqMatchPolicy policy{
                        PdqDistance{radius},
                        PdqQuality{quality},
                        Coverage{qmin},
                        Coverage{cmin}
                    };
                    const auto ids = find_video_candidates(flat, catalog, record.signature, policy);

                    EXPECT_EQ(find_video_candidates(mih, catalog, record.signature, policy), ids);
                    EXPECT_EQ(
                        verify_video_candidates(record.id, record.signature, ids, catalog, policy),
                        oracle_matches(record.id, record.signature, catalog, policy)
                    );
                }
            }
        }
    }

    EXPECT_THROW(
        (void)verify_video_candidates(
            VideoId{0},
            records[0].signature,
            std::array{VideoId{1000}},
            catalog
        ),
        std::out_of_range
    );

    FlatPdqIndex stale;
    stale.insert(FingerprintId{99999}, records[0].signature.frames[0].hash);

    EXPECT_THROW(
        (void)find_video_candidates(
            stale,
            catalog,
            records[0].signature,
            VpdqMatchPolicy{PdqDistance{256}, PdqQuality{0}}
        ),
        std::out_of_range
    );

    const auto &query = records[17].signature;
    const auto expected = oracle_matches(VideoId{17}, query, catalog, {});
    std::array<std::future<std::vector<VideoMatch>>, 4> readers;

    for (auto &reader : readers) {
        reader = std::async(std::launch::async, [&] {
            return verify_video_candidates(
                VideoId{17},
                query,
                find_video_candidates(mih, catalog, query),
                catalog
            );
        });
    }

    for (auto &reader : readers) {
        EXPECT_EQ(reader.get(), expected);
    }
}

} // namespace
