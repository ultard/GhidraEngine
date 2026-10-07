#include <GhidraEngine/core/media.hpp>
#include <GhidraEngine/version.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <type_traits>

using namespace GhidraEngine;

static_assert(!std::is_convertible_v<MediaId, VideoId>);
static_assert(!std::is_convertible_v<FingerprintId, MediaId>);
static_assert(!std::is_convertible_v<std::uint64_t, MediaId>);
static_assert(!std::is_convertible_v<PdqQuality, PdqDistance>);
static_assert(!std::is_convertible_v<double, Coverage>);
static_assert(!std::is_convertible_v<std::int64_t, Timestamp>);
static_assert(std::is_trivially_copyable_v<PdqHash>);
static_assert(std::is_trivially_copyable_v<Blake3Digest>);
static_assert(PdqDistance{256}.value() == 256);
static_assert(PdqQuality{100}.value() == 100);

TEST(Core, IdsPreserveZeroAndFullUint64Range) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();

    EXPECT_EQ(MediaId{}.value, 0U);
    EXPECT_EQ(MediaId{maximum}.value, maximum);
    EXPECT_EQ(VideoId{maximum}.value, maximum);
    EXPECT_EQ(FingerprintId{maximum}.value, maximum);
    EXPECT_LT(MediaId{}, MediaId{maximum});
    EXPECT_FALSE(version().empty());
}

TEST(Core, QualityAndDistanceValidateBeforeNarrowing) {
    for (int value = 0; value <= 256; ++value) {
        EXPECT_EQ(PdqDistance{value}.value(), value);

        if (value <= 100) {
            EXPECT_EQ(PdqQuality{value}.value(), value);
        }
    }

    for (auto value :
         {-1LL,
          101LL,
          256LL,
          std::numeric_limits<long long>::min(),
          std::numeric_limits<long long>::max()}) {
        EXPECT_THROW((void)PdqQuality{value}, std::invalid_argument);
    }

    for (auto value :
         {-1LL,
          257LL,
          65536LL,
          std::numeric_limits<long long>::min(),
          std::numeric_limits<long long>::max()}) {
        EXPECT_THROW((void)PdqDistance{value}, std::invalid_argument);
    }
}

TEST(Core, CoverageRequiresFiniteUnitFraction) {
    for (double value : {0.0, 0.25, 1.0}) {
        EXPECT_EQ(Coverage{value}.value(), value);
    }

    for (double value :
         {-0.01,
          1.01,
          std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW((void)Coverage{value}, std::invalid_argument);
    }

    EXPECT_EQ(Timestamp{-123}.count(), -123);
}
