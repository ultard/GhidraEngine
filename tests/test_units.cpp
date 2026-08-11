#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ghidraengine/ghidraengine.hpp"
#include "core/enumerate.hpp"
#include "core/glob.hpp"
#include "core/thread_pool.hpp"
#include "hash/dct.hpp"
#include "index/cluster.hpp"
#include "index/mih_index.hpp"

using namespace ghidraengine;

TEST_CASE("glob matching", "[glob]") {
    CHECK(glob_match("*.jpg", "photo.jpg"));
    CHECK_FALSE(glob_match("*.jpg", "photo.png"));
    CHECK(glob_match("photo?.jpg", "photo1.jpg"));
    CHECK_FALSE(glob_match("photo?.jpg", "photo12.jpg"));
    CHECK(glob_match("*.[jp]*", "photo.jpg"));
    CHECK(glob_match("*.[!p]*", "photo.jpg"));
    CHECK_FALSE(glob_match("*.[!j]*", "photo.jpg"));

    SECTION("a single star does not cross directory separators") {
        CHECK_FALSE(glob_match("/photos/*.jpg", "/photos/2024/a.jpg"));
        CHECK(glob_match("/photos/*.jpg", "/photos/a.jpg"));
    }

    SECTION("double star crosses separators and matches zero directories") {
        CHECK(glob_match("/photos/**/*.jpg", "/photos/2024/summer/a.jpg"));
        CHECK(glob_match("/photos/**/*.jpg", "/photos/a.jpg"));
        CHECK(glob_match("**/thumbs/**", "/a/b/thumbs/c/d.jpg"));
    }

    SECTION("separators are interchangeable") {
        CHECK(glob_match("C:/photos/**", R"(C:\photos\2024\a.jpg)"));
    }

    SECTION("pathological patterns terminate") {
        CHECK_FALSE(glob_match("*a*a*a*a*a*a*b", std::string(64, 'a')));
    }
}

TEST_CASE("format identification from magic bytes", "[magic]") {
    const auto classify = [](std::initializer_list<int> bytes) {
        std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
        data.resize(std::max<std::size_t>(data.size(), 16), 0);
        return classify_header(data);
    };

    CHECK(classify({0xFF, 0xD8, 0xFF, 0xE0}) == MediaKind::Image);
    CHECK(classify({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}) == MediaKind::Image);
    CHECK(classify({'G', 'I', 'F', '8', '9', 'a'}) == MediaKind::Image);
    CHECK(classify({'B', 'M', 0, 0}) == MediaKind::Image);
    CHECK(classify({'I', 'I', 0x2A, 0x00}) == MediaKind::Image);
    CHECK(classify({0x1A, 0x45, 0xDF, 0xA3}) == MediaKind::Video);
    CHECK(classify({'F', 'L', 'V', 0x01}) == MediaKind::Video);

    SECTION("RIFF payload decides between WebP and AVI") {
        std::vector<std::uint8_t> webp = {'R', 'I', 'F', 'F', 0, 0, 0, 0,
                                          'W', 'E', 'B', 'P', 0, 0, 0, 0};
        std::vector<std::uint8_t> avi = {'R', 'I', 'F', 'F', 0, 0, 0, 0,
                                         'A', 'V', 'I', ' ', 0, 0, 0, 0};
        CHECK(classify_header(webp) == MediaKind::Image);
        CHECK(classify_header(avi) == MediaKind::Video);
    }

    SECTION("ISO base media brand separates HEIC from MP4") {
        std::vector<std::uint8_t> heic = {0, 0, 0, 0x18, 'f', 't', 'y', 'p',
                                          'h', 'e', 'i', 'c', 0, 0, 0, 0};
        std::vector<std::uint8_t> avif = {0, 0, 0, 0x18, 'f', 't', 'y', 'p',
                                          'a', 'v', 'i', 'f', 0, 0, 0, 0};
        std::vector<std::uint8_t> mp4 = {0, 0, 0, 0x18, 'f', 't', 'y', 'p',
                                         'i', 's', 'o', 'm', 0, 0, 0, 0};
        CHECK(classify_header(heic) == MediaKind::Image);
        CHECK(classify_header(avif) == MediaKind::Image);
        CHECK(classify_header(mp4) == MediaKind::Video);
    }

    SECTION("a lone 0x47 is not a transport stream") {
        std::vector<std::uint8_t> data(400, 0);
        data[0] = 0x47;
        CHECK(classify_header(data) == MediaKind::Unknown);

        data[188] = 0x47;
        data[376] = 0x47;
        CHECK(classify_header(data) == MediaKind::Video);
    }

    SECTION("extension prefilter") {
        CHECK(extension_is_candidate("a/b/photo.JPG"));
        CHECK(extension_is_candidate("movie.MKV"));
        CHECK(extension_is_candidate("raw.cr2"));
        CHECK_FALSE(extension_is_candidate("notes.txt"));
        CHECK_FALSE(extension_is_candidate("no_extension"));
    }
}

TEST_CASE("SIMD DCT kernels agree with the scalar reference", "[dct][simd]") {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> pixels(0.0F, 255.0F);

    std::vector<Dct16Fn> kernels;
    kernels.push_back(&dct16_scalar);
#if defined(GHIDRAENGINE_X86_SIMD)
    if (active_backend() == SimdBackend::Avx2) {
        kernels.push_back(&dct16_avx2);
    }
    kernels.push_back(&dct16_sse2);
#endif
#if defined(GHIDRAENGINE_NEON_SIMD)
    kernels.push_back(&dct16_neon);
#endif

    INFO("active backend: " << active_simd_backend());
    REQUIRE(kernels.size() >= 1);

    const auto hash_of = [](const float* coefficients) {
        std::vector<float> ac;
        for (std::size_t v = 0; v < 8; ++v) {
            for (std::size_t u = 0; u < 8; ++u) {
                if (v != 0 || u != 0) {
                    ac.push_back(coefficients[v * kDctOutputSize + u]);
                }
            }
        }
        std::nth_element(ac.begin(), ac.begin() + ac.size() / 2, ac.end());
        const float median = ac[ac.size() / 2];

        std::uint64_t hash = 0;
        for (std::size_t v = 0; v < 8; ++v) {
            for (std::size_t u = 0; u < 8; ++u) {
                if (coefficients[v * kDctOutputSize + u] > median) {
                    hash |= 1ULL << (v * 8 + u);
                }
            }
        }
        return hash;
    };

    for (int trial = 0; trial < 64; ++trial) {
        alignas(64) float input[kDctInputCount];
        for (float& value : input) {
            value = pixels(rng);
        }

        alignas(64) float reference[kDctOutputCount];
        dct16_scalar(input, reference);

        for (const Dct16Fn kernel : kernels) {
            alignas(64) float actual[kDctOutputCount];
            kernel(input, actual);

            for (std::size_t i = 0; i < kDctOutputCount; ++i) {
                const float scale = std::max(1.0F, std::abs(reference[i]));
                CHECK(std::abs(actual[i] - reference[i]) / scale < 1e-3F);
            }

            CHECK(hash_of(actual) == hash_of(reference));
        }
    }
}

TEST_CASE("MIH returns exactly the same set as brute force", "[mih]") {
    std::mt19937_64 rng(987654321);

    for (const std::uint32_t threshold : {0U, 3U, 8U, 10U, 12U, 16U, 20U, 24U}) {
        std::vector<std::uint64_t> codes(4000);
        for (std::uint64_t& code : codes) {
            code = rng();
        }
        for (std::size_t i = 0; i < 200; ++i) {
            std::uint64_t base = codes[i];
            const int flips = static_cast<int>(rng() % (threshold + 1));
            for (int f = 0; f < flips; ++f) {
                base ^= 1ULL << (rng() % 64);
            }
            codes[codes.size() - 1 - i] = base;
        }

        MihIndex index;
        index.build(codes);

        std::vector<std::uint32_t> candidates;
        std::vector<std::uint32_t> visited;
        std::uint32_t epoch = 0;

        for (std::size_t query = 0; query < codes.size(); query += 7) {
            index.query(codes[query], threshold, candidates, visited, epoch);
            std::set<std::uint32_t> from_index(candidates.begin(), candidates.end());

            std::set<std::uint32_t> from_brute_force;
            for (std::uint32_t i = 0; i < codes.size(); ++i) {
                if (std::popcount(codes[query] ^ codes[i]) <= static_cast<int>(threshold)) {
                    from_brute_force.insert(i);
                }
            }

            INFO("threshold " << threshold << ", query " << query);
            REQUIRE(from_index == from_brute_force);
        }
    }
}

TEST_CASE("union-find and clustering", "[cluster]") {
    SECTION("transitive grouping merges chains") {
        const std::vector<MatchPair> pairs = {{0, 1, 5}, {1, 2, 5}, {5, 6, 5}};
        const auto groups = group_transitive(8, pairs);

        REQUIRE(groups.size() == 2);
        CHECK(groups[0] == std::vector<std::uint32_t>{0, 1, 2});
        CHECK(groups[1] == std::vector<std::uint32_t>{5, 6});
    }

    SECTION("strict grouping refuses to chain") {
        const std::vector<MatchPair> pairs = {{0, 1, 9}, {1, 2, 9}};

        const auto transitive = group_transitive(4, pairs);
        REQUIRE(transitive.size() == 1);
        CHECK(transitive[0].size() == 3);

        const auto strict = group_strict(4, pairs);
        REQUIRE(strict.size() == 1);
        CHECK(strict[0] == std::vector<std::uint32_t>{0, 1});
    }

    SECTION("keeper policies") {
        std::vector<KeeperInfo> info(3);
        info[0] = KeeperInfo{1000, 500, 100, 20};
        info[1] = KeeperInfo{4000, 200, 300, 10};
        info[2] = KeeperInfo{4000, 900, 200, 30};

        const std::vector<std::uint32_t> members = {0, 1, 2};

        CHECK(choose_keeper(members, info, KeeperPolicy::HighestResolution) == 2);
        CHECK(choose_keeper(members, info, KeeperPolicy::LargestFile) == 2);
        CHECK(choose_keeper(members, info, KeeperPolicy::OldestModified) == 0);
        CHECK(choose_keeper(members, info, KeeperPolicy::NewestModified) == 1);
        CHECK(choose_keeper(members, info, KeeperPolicy::ShortestPath) == 1);
    }
}

TEST_CASE("parallel_for survives a throwing body", "[pool]") {
    ThreadPool pool(4);

    SECTION("the exception reaches the caller") {
        std::atomic<int> ran{0};
        CHECK_THROWS_AS(parallel_for(pool, 0, 1000,
                                     [&](std::size_t index) {
                                         ran.fetch_add(1, std::memory_order_relaxed);
                                         if (index == 500) {
                                             throw std::runtime_error("boom");
                                         }
                                     }),
                        std::runtime_error);
        CHECK(ran.load() > 0);
    }

    SECTION("the pool is still usable afterwards") {
        std::atomic<int> total{0};
        parallel_for(pool, 0, 100,
                     [&](std::size_t) { total.fetch_add(1, std::memory_order_relaxed); });
        CHECK(total.load() == 100);
    }
}

TEST_CASE("the version string matches the version constants", "[version]") {
    const std::string expected = std::to_string(Version::major) + "." +
                                 std::to_string(Version::minor) + "." +
                                 std::to_string(Version::patch);
    CHECK(std::string(version_string()) == expected);
}

TEST_CASE("hamming distance", "[hash]") {
    CHECK(hamming_distance(0ULL, 0ULL) == 0);
    CHECK(hamming_distance(0ULL, ~0ULL) == 64);
    CHECK(hamming_distance(0b1011ULL, 0b1101ULL) == 2);

    const Hash256 a{0, 0, 0, 0};
    const Hash256 b{~0ULL, ~0ULL, ~0ULL, ~0ULL};
    CHECK(hamming_distance(a, a) == 0);
    CHECK(hamming_distance(a, b) == 256);
}
