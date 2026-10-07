#include <GhidraEngine/index/flat_pdq.hpp>
#include <GhidraEngine/index/mih_pdq.hpp>
#include <GhidraEngine/index/video.hpp>
#include <GhidraEngine/match/video.hpp>

#include "../tests/support/test_support.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <random>
#include <utility>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

enum class Distribution { Near, Dense, Disjoint, Correlated };

void Compare(benchmark::State &state) {
    std::mt19937_64 random(0x76706471);

    VpdqSignature query, candidate;
    const auto distribution = static_cast<Distribution>(state.range(2));

    for (std::int64_t i = 0; i < state.range(0); ++i) {
        auto hash = distribution == Distribution::Dense
                        ? PdqHash{{static_cast<std::uint64_t>(i), 0, 0, 0}}
                        : random_hash(random);

        if (distribution == Distribution::Correlated) {
            hash.words[0] = hash.words[1] = 0;
        }

        query.frames.push_back({hash, PdqQuality{100}, Timestamp{i}});
        auto changed = hash;

        if (distribution == Distribution::Disjoint) {
            changed = random_hash(random);
        } else if (distribution == Distribution::Correlated) {
            changed.words[0] = ~std::uint64_t{0};
        } else {
            changed.words[0] ^= 0x7fffffff;
        }

        candidate.frames.push_back({changed, PdqQuality{100}, Timestamp{-i}});
    }

    const PdqMatchPolicy policy{
        PdqDistance{state.range(1)},
        PdqQuality{50}
    };

    const bool positive = distribution == Distribution::Near || distribution == Distribution::Dense;
    const bool matches = policy.max_distance.value() >= 31 && positive;

    const VpdqComparison expected{
        Coverage{matches ? 1.0 : 0.0},
        Coverage{matches ? 1.0 : 0.0},
        query.frames.size(),
        candidate.frames.size()
    };

    if (compare_vpdq(query, candidate, policy) != expected) {
        state.SkipWithError("vPDQ comparison differs from constructed coverage");
        return;
    }

    for (auto _ : state) {
        auto result = compare_vpdq(query, candidate, policy);
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations() * state.range(0));
}

void Arguments(benchmark::Benchmark *bench) {
    for (int count : {32, 512, 2048}) {
        for (int radius : {0, 31, 32, 256}) {
            bench->Args({count, radius, 0});
        }

        for (int distribution : {1, 2, 3}) {
            bench->Args({count, 31, distribution});
        }
    }

    bench->Args({512, 30, 0});
}

BENCHMARK(Compare)
    ->Apply(Arguments)
    ->ArgNames({"frames", "radius", "distribution"});

template <class Index>
void VideoCandidates(benchmark::State &state) {
    std::mt19937_64 random(0x766964656f);
    std::vector<VideoSignatureRecord> records;

    const auto videos = static_cast<std::size_t>(state.range(0)) / 100;
    records.reserve(videos);

    for (std::size_t i = 0; i < videos; ++i) {
        VpdqSignature signature;
        signature.frames.reserve(100);

        for (unsigned frame = 0; frame < 100; ++frame) {
            signature.frames.push_back(
                {random_hash(random), PdqQuality{100}, Timestamp{frame * 1000000LL}}
            );
        }

        records.push_back({VideoId{i}, std::move(signature)});
    }

    const VideoFingerprintCatalog catalog(records);
    const auto entries = catalog.index_entries();
    const Index index(entries);
    VpdqSignature query;

    for (unsigned i = 0; i < 10; ++i) {
        query.frames.push_back(catalog.at(VideoId{i}).frames.front());
    }

    const auto expected = find_video_candidates(FlatPdqIndex(entries), catalog, query);

    if (expected.size() != 10 || find_video_candidates(index, catalog, query) != expected) {
        state.SkipWithError("Video candidates differ from Flat or lost constructed hits");

        return;
    }

    for (auto _ : state) {
        auto ids = find_video_candidates(index, catalog, query);
        benchmark::DoNotOptimize(ids);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["candidates"] = static_cast<double>(expected.size());
}

BENCHMARK_TEMPLATE(VideoCandidates, FlatPdqIndex)
    ->Arg(10000)
    ->Arg(100000)
    ->ArgName("indexed_frames");

BENCHMARK_TEMPLATE(VideoCandidates, MihPdqIndex)
    ->Arg(10000)
    ->Arg(100000)
    ->ArgName("indexed_frames");

void VideoVerification(benchmark::State &state) {
    std::mt19937_64 random(0x766572696679);
    VpdqSignature query;

    for (std::int64_t i = 0; i < state.range(0); ++i) {
        query.frames.push_back({random_hash(random), PdqQuality{100}, Timestamp{i}});
    }

    auto candidate = query;

    for (auto &frame : candidate.frames) {
        frame.hash.words[0] ^= 0x7fffffff;
    }

    std::vector<VideoSignatureRecord> records;
    std::vector<VideoId> ids;

    for (unsigned i = 1; i <= 16; ++i) {
        records.push_back({VideoId{i}, candidate});
        ids.push_back(VideoId{i});
    }

    const VideoFingerprintCatalog catalog(records);
    const auto expected = verify_video_candidates(VideoId{0}, query, ids, catalog);

    if (expected.size() != ids.size()) {
        state.SkipWithError("Video verification lost a constructed match");

        return;
    }

    for (auto _ : state) {
        auto matches = verify_video_candidates(VideoId{0}, query, ids, catalog);
        benchmark::DoNotOptimize(matches);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["matches"] = static_cast<double>(expected.size());
}

BENCHMARK(VideoVerification)
    ->Arg(64)
    ->Arg(512)
    ->ArgName("frames");

}
