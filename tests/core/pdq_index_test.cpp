#include <GhidraEngine/index/mih_pdq.hpp>

#include "../support/test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <future>
#include <limits>
#include <ranges>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;
static_assert(PdqRangeIndex<FlatPdqIndex> && PdqRangeIndex<MihPdqIndex>);
static_assert(!PdqRangeIndex<PdqHash>);

std::vector<PdqHit>
brute_search(std::span<const PdqIndexEntry> entries, const PdqHash &query, unsigned radius) {
    std::vector<PdqHit> hits;

    for (const auto &entry : entries) {
        const auto distance = bit_distance(query, entry.hash);

        if (distance <= radius) {
            hits.push_back({entry.id, PdqDistance{distance}});
        }
    }

    std::ranges::sort(hits, {}, &PdqHit::id);

    return hits;
}

std::vector<PdqIndexEntry> random_entries(std::size_t count) {
    std::mt19937_64 random(0x696e646578);
    std::vector<PdqIndexEntry> entries;

    for (std::size_t i = 0; i < count; ++i) {
        entries.push_back({FingerprintId{1000 + i * 17}, random_hash(random)});
    }

    return entries;
}

template <class Index>
class PdqIndexTest : public testing::Test {};

using IndexTypes = testing::Types<FlatPdqIndex, MihPdqIndex>;
TYPED_TEST_SUITE(PdqIndexTest, IndexTypes);

TYPED_TEST(PdqIndexTest, EveryDistanceIsInclusiveAndResultsAreSortedById) {
    std::vector<PdqIndexEntry> entries;

    for (unsigned distance = 0; distance <= 256; ++distance) {
        entries.push_back({FingerprintId{256 - distance}, prefix_hash(distance)});
    }

    const TypeParam index(entries);

    for (std::uint16_t radius = 0; radius <= 256; ++radius) {
        SCOPED_TRACE(radius);
        const auto hits = index.search_within({}, radius);

        ASSERT_EQ(hits.size(), radius + 1U);

        for (std::size_t i = 0; i < hits.size(); ++i) {
            EXPECT_EQ(hits[i].id.value, 256U - radius + i);
            EXPECT_EQ(hits[i].distance.value(), radius - i);
        }
    }
}

TYPED_TEST(PdqIndexTest, EmptyInputsInvalidRadiiAndDuplicateIdsHaveDefinedBehavior) {
    TypeParam index;

    EXPECT_EQ(index.size(), 0U);
    EXPECT_TRUE(index.search_within({}, 256).empty());
    EXPECT_THROW((void)index.search_within({}, 257), std::invalid_argument);
    EXPECT_THROW((void)index.search_within({}, 65535), std::invalid_argument);

    index.insert(FingerprintId{0}, {});
    index.insert(FingerprintId{std::numeric_limits<std::uint64_t>::max()}, {});
    const auto before = index.search_within({}, 0);

    ASSERT_EQ(before.size(), 2U);
    EXPECT_THROW(index.insert(FingerprintId{0}, prefix_hash(1)), std::invalid_argument);
    EXPECT_EQ(index.search_within({}, 0), before);

    const std::array duplicate{
        PdqIndexEntry{FingerprintId{7}, {}},
        PdqIndexEntry{FingerprintId{7}, prefix_hash(2)}
    };

    EXPECT_THROW((void)TypeParam{duplicate}, std::invalid_argument);
}

TYPED_TEST(PdqIndexTest, BulkLoadingOwnsInputAndUnorderedInsertionsMatchOracle) {
    auto entries = random_entries(2048);
    std::ranges::reverse(entries);
    const auto original = entries;
    TypeParam index(entries);
    entries.front().hash = {};

    EXPECT_EQ(
        index.search_within(original.front().hash, 0),
        brute_search(original, original.front().hash, 0)
    );

    entries = original;

    for (const auto &entry :
         {PdqIndexEntry{FingerprintId{0}, original[5].hash},
          PdqIndexEntry{FingerprintId{1001}, original[9].hash},
          PdqIndexEntry{FingerprintId{90000}, original[0].hash}}) {
        index.insert(entry.id, entry.hash);
        entries.push_back(entry);

        for (unsigned radius : {0U, 15U, 16U, 31U, 32U, 47U, 48U, 128U, 256U}) {
            EXPECT_EQ(
                index.search_within(entry.hash, static_cast<std::uint16_t>(radius)),
                brute_search(entries, entry.hash, radius)
            );
        }
    }

    EXPECT_EQ(index.size(), entries.size());
}

TYPED_TEST(PdqIndexTest, CopyMoveAndReserveKeepIndependentValues) {
    const auto entries = random_entries(1024);
    TypeParam original(entries);
    auto copy = original;
    copy.reserve(2048);
    copy.insert(FingerprintId{1}, entries[0].hash);

    EXPECT_EQ(original.size(), entries.size());

    const auto expected = copy.search_within(entries[0].hash, 0);
    TypeParam assigned;
    assigned = copy;
    TypeParam moved(std::move(copy));

    EXPECT_EQ(moved.search_within(entries[0].hash, 0), expected);
    EXPECT_EQ(assigned.search_within(entries[0].hash, 0), expected);

    assigned = std::move(moved);

    EXPECT_EQ(assigned.search_within(entries[0].hash, 0), expected);

    assigned.reserve(0);

    EXPECT_THROW(assigned.reserve(std::numeric_limits<std::size_t>::max()), std::length_error);

    assigned.insert(FingerprintId{2}, {});

    EXPECT_EQ(assigned.size(), entries.size() + 2);
}

TYPED_TEST(PdqIndexTest, RandomAndClusteredQueriesMatchIndependentOracle) {
    auto entries = random_entries(4096);
    std::mt19937_64 random(123);
    const auto center = random_hash(random);

    for (unsigned bits = 0; bits <= 64; ++bits) {
        auto hash = center;

        for (unsigned bit = 0; bit < bits; ++bit) {
            hash.words[bit / 64] ^= std::uint64_t{1} << (bit % 64);
        }

        entries.push_back({FingerprintId{bits}, hash});
    }

    const TypeParam index(entries);

    for (const auto &query : {center, entries[101].hash, random_hash(random)}) {
        for (unsigned radius : {0U, 1U, 15U, 16U, 31U, 32U, 47U, 48U, 127U, 128U, 255U, 256U}) {
            SCOPED_TRACE(radius);

            EXPECT_EQ(
                index.search_within(query, static_cast<std::uint16_t>(radius)),
                brute_search(entries, query, radius)
            );
        }
    }
}

TYPED_TEST(PdqIndexTest, LargeResultsAreNeverCappedAndConstQueriesAreConcurrent) {
    std::vector<PdqIndexEntry> entries;

    for (unsigned i = 0; i < 10000; ++i) {
        entries.push_back({FingerprintId{i}, {}});
    }

    const TypeParam index(entries);
    std::array<std::future<std::vector<PdqHit>>, 4> readers;

    for (auto &reader : readers) {
        reader = std::async(std::launch::async, [&] {
            return index.search_within({}, 0);
        });
    }

    const auto expected = brute_search(entries, {}, 0);

    ASSERT_EQ(expected.size(), 10000U);

    for (auto &reader : readers) {
        EXPECT_EQ(reader.get(), expected);
    }
}

std::vector<PdqIndexEntry> far_background() {
    std::vector<PdqIndexEntry> entries(400000);

    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i] = {FingerprintId{1000000 + i}, prefix_hash(256)};
    }

    return entries;
}

TEST(Mih, BalancedBoundaryHashesExerciseAcceleratedRadii) {
    auto entries = far_background();
    std::vector<PdqIndexEntry> boundary;

    for (unsigned distance = 0; distance < 48; ++distance) {
        PdqHash hash;

        for (unsigned slot = 0; slot < 16; ++slot) {
            const auto bits = distance / 16 + (slot < distance % 16 ? 1U : 0U);

            for (unsigned bit = 0; bit < bits; ++bit) {
                hash.words[slot / 4] |= std::uint64_t{1} << (16 * (slot % 4) + 15 - bit);
            }
        }

        boundary.push_back({FingerprintId{distance}, hash});
    }

    entries.insert(entries.end(), boundary.begin(), boundary.end());
    const MihPdqIndex index(entries);

    for (std::uint16_t radius = 0; radius <= 48; ++radius) {
        SCOPED_TRACE(radius);
        MihSearchStats stats;

        EXPECT_EQ(index.search_within({}, radius, &stats), brute_search(boundary, {}, radius));
        EXPECT_EQ(stats.used_flat, radius == 48);

        if (radius < 48) {
            EXPECT_LT(stats.candidates_verified, entries.size());
        }
    }
}

TEST(Mih, EverySlotAndEveryTwoBitProbeMaskRetainsBoundaryHits) {
    auto entries = far_background();
    std::vector<PdqIndexEntry> boundary;

    for (unsigned slot = 0; slot < 16; ++slot) {
        for (unsigned a = 0; a < 16; ++a) {
            for (unsigned b = a + 1; b < 16; ++b) {
                PdqHash hash{
                    {0x0007000700070007ULL,
                     0x0007000700070007ULL,
                     0x0007000700070007ULL,
                     0x0007000700070007ULL}
                };
                const auto shift = 16 * (slot % 4);
                hash.words[slot / 4] &= ~(std::uint64_t{0xffff} << shift);
                hash.words[slot / 4] |= ((std::uint64_t{1} << a) | (std::uint64_t{1} << b))
                                        << shift;
                boundary.push_back({FingerprintId{boundary.size()}, hash});
            }
        }
    }

    entries.insert(entries.end(), boundary.begin(), boundary.end());
    const MihPdqIndex index(entries);
    MihSearchStats stats;

    EXPECT_EQ(index.search_within({}, 47, &stats), brute_search(boundary, {}, 47));
    EXPECT_FALSE(stats.used_flat);
    EXPECT_TRUE(index.search_within({}, 46).empty());

    entries = far_background();
    boundary.clear();

    for (unsigned slot = 0; slot < 16; ++slot) {
        for (unsigned bit = 0; bit < 16; ++bit) {
            PdqHash hash{
                {0x0003000300030003ULL,
                 0x0003000300030003ULL,
                 0x0003000300030003ULL,
                 0x0003000300030003ULL}
            };
            const auto shift = 16 * (slot % 4);
            hash.words[slot / 4] &= ~(std::uint64_t{0xffff} << shift);
            hash.words[slot / 4] |= std::uint64_t{1} << (shift + bit);
            boundary.push_back({FingerprintId{boundary.size()}, hash});
        }
    }

    entries.insert(entries.end(), boundary.begin(), boundary.end());
    const MihPdqIndex singles(entries);

    EXPECT_EQ(singles.search_within({}, 31, &stats), brute_search(boundary, {}, 31));
    EXPECT_FALSE(stats.used_flat);
    EXPECT_TRUE(singles.search_within({}, 30).empty());
}

TEST(Mih, FullHashVerificationRemovesFalsePositivesAndStatsResetPerQuery) {
    auto entries = random_entries(50000);
    auto false_positive = prefix_hash(256);
    false_positive.words[0] &= ~std::uint64_t{0xffff};
    entries.push_back({FingerprintId{0}, false_positive});
    entries.push_back({FingerprintId{1}, {}});
    entries.push_back({FingerprintId{2}, {}});
    const MihPdqIndex index(entries);
    MihSearchStats stats{1, 2, 3, true};

    EXPECT_EQ(
        index.search_within({}, 31, &stats),
        (std::vector<PdqHit>{
            {FingerprintId{1}, PdqDistance{0}},
            {FingerprintId{2}, PdqDistance{0}}
        })
    );
    EXPECT_FALSE(stats.used_flat);
    EXPECT_GE(stats.candidates_verified, 3U);
    EXPECT_EQ(index.search_within({}, 256, &stats).size(), entries.size());
    EXPECT_TRUE(stats.used_flat);
    EXPECT_EQ(stats.slot_lookups, 0U);
    EXPECT_EQ(stats.candidates_verified, entries.size());

    auto source = index;

    EXPECT_THROW(source.reserve(std::numeric_limits<std::size_t>::max()), std::length_error);

    const MihPdqIndex moved(std::move(source));

    EXPECT_EQ(source.size(), 0U);
    EXPECT_EQ(source.storage_bytes(), 0U);

    source.insert(FingerprintId{0}, {});

    EXPECT_EQ(source.search_within({}, 0).size(), 1U);
    EXPECT_EQ(moved.size(), index.size());
}

TEST(Mih, ExpensiveBucketsFallBackAndAcceleratedConcurrentQueriesKeepLocalStats) {
    std::vector<PdqIndexEntry> dense(10000);

    for (std::size_t i = 0; i < dense.size(); ++i) {
        dense[i].id = FingerprintId{i};
    }

    const MihPdqIndex dense_index(dense);

    for (unsigned radius : {0U, 15U, 31U, 32U, 47U, 48U, 256U}) {
        MihSearchStats stats;

        EXPECT_EQ(
            dense_index.search_within({}, static_cast<std::uint16_t>(radius), &stats).size(),
            dense.size()
        );
        EXPECT_TRUE(stats.used_flat);
        EXPECT_EQ(stats.posting_visits, 0U);
    }

    const auto entries = random_entries(10000);
    const MihPdqIndex index(entries);
    const auto expected = brute_search(entries, entries[0].hash, 16);
    std::array<std::future<std::vector<PdqHit>>, 4> readers;

    for (auto &reader : readers) {
        reader = std::async(std::launch::async, [&] {
            MihSearchStats stats;
            auto hits = index.search_within(entries[0].hash, 16, &stats);

            EXPECT_FALSE(stats.used_flat);

            return hits;
        });
    }

    for (auto &reader : readers) {
        EXPECT_EQ(reader.get(), expected);
    }
}

TEST(Mih, ReverseInsertionKeepsPostingPositionsStableAndIdsSorted) {
    const auto entries = random_entries(4096);
    MihPdqIndex index;
    index.reserve(entries.size());

    for (const auto &entry : entries | std::views::reverse) {
        index.insert(entry.id, entry.hash);
    }

    for (unsigned radius : {0U, 16U, 31U, 47U, 256U}) {
        EXPECT_EQ(
            index.search_within(entries[100].hash, static_cast<std::uint16_t>(radius)),
            brute_search(entries, entries[100].hash, radius)
        );
    }

    EXPECT_THROW(index.insert(entries[100].id, {}), std::invalid_argument);
    EXPECT_EQ(index.size(), entries.size());
}

TEST(Mih, OrderedRecordsKeepCompactStorageUntilAnUnorderedInsertion) {
    auto entries = random_entries(4096);
    const MihPdqIndex bulk(entries);
    MihPdqIndex index;
    index.reserve(entries.size());

    for (const auto &entry : entries) {
        index.insert(entry.id, entry.hash);
    }

    const auto compact_bytes = index.storage_bytes();

    EXPECT_EQ(compact_bytes, bulk.storage_bytes());
    EXPECT_EQ(compact_bytes, (1U << 23U) + entries.size() * (sizeof(PdqIndexEntry) + 64));
    EXPECT_THROW(index.insert(entries[0].id, {}), std::invalid_argument);
    EXPECT_EQ(index.storage_bytes(), compact_bytes);

    const PdqIndexEntry extra{FingerprintId{0}, entries[100].hash};
    index.insert(extra.id, extra.hash);
    entries.push_back(extra);

    for (unsigned radius : {0U, 16U, 31U, 47U, 256U}) {
        EXPECT_EQ(
            index.search_within(extra.hash, static_cast<std::uint16_t>(radius)),
            brute_search(entries, extra.hash, radius)
        );
    }

    EXPECT_THROW(index.insert(entries[0].id, {}), std::invalid_argument);
    EXPECT_THROW(index.insert(extra.id, {}), std::invalid_argument);
    EXPECT_EQ(index.size(), entries.size());
}

} // namespace
