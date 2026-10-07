#include <GhidraEngine/image/fingerprint.hpp>
#include <GhidraEngine/index/flat_pdq.hpp>
#include <GhidraEngine/index/mih_pdq.hpp>
#include <GhidraEngine/match/image.hpp>

#include "../tests/support/test_support.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <random>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

TransformPolicy transform_policy(std::int64_t variants) {
    switch (variants) {
        case 1:
            return TransformPolicy::OriginalOnly;
        case 4:
            return TransformPolicy::Rotations;
        default:
            return TransformPolicy::Dihedral;
    }
}

void ImageVariants(benchmark::State &state) {
    const auto image = pattern_image(
        static_cast<std::size_t>(state.range(0)),
        static_cast<std::size_t>(state.range(1))
    );
    const auto policy = transform_policy(state.range(2));

    for (auto _ : state) {
        auto signature = fingerprint_image(image, policy);
        benchmark::DoNotOptimize(signature);
    }

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(ImageVariants)
    ->Args({512, 512, 4})
    ->Args({512, 512, 8})
    ->Args({1920, 1080, 4})
    ->Args({1920, 1080, 8})
    ->ArgNames({"width", "height", "variants"});

template <class Index>
void ImageCandidates(benchmark::State &state) {
    const auto query = fingerprint_image(
        pattern_image(64, 64), transform_policy(state.range(1))
    );
    std::mt19937_64 random(0x63616e64);
    std::vector<PdqIndexEntry> entries(static_cast<std::size_t>(state.range(0)));

    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i] = {FingerprintId{i}, random_hash(random)};
    }

    for (std::size_t i = 0; i < query.variants.size(); ++i) {
        entries[i].hash = query.variants[i].hash;
    }

    const Index index(entries);
    const PdqDistance radius{state.range(2)};
    const auto expected = find_image_candidates(FlatPdqIndex(entries), query, radius);

    if (expected.empty() || find_image_candidates(index, query, radius) != expected) {
        state.SkipWithError("Image candidates differ from Flat or contain no hits");

        return;
    }

    for (auto _ : state) {
        auto ids = find_image_candidates(index, query, radius);
        benchmark::DoNotOptimize(ids);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["candidates"] = static_cast<double>(expected.size());
}

void ImageCandidateArguments(benchmark::Benchmark *bench) {
    for (int count : {1000, 100000}) {
        for (int variants : {1, 8}) {
            bench->Args({count, variants, 31});
        }
    }

    bench->Args({1000, 8, 256});
    bench->Args({1000000, 8, 31});
}

BENCHMARK_TEMPLATE(ImageCandidates, FlatPdqIndex)
    ->Apply(ImageCandidateArguments)
    ->ArgNames({"N", "variants", "radius"});

BENCHMARK_TEMPLATE(ImageCandidates, MihPdqIndex)
    ->Apply(ImageCandidateArguments)
    ->ArgNames({"N", "variants", "radius"});

void ImageVerification(benchmark::State &state) {
    const auto query = fingerprint_image(
        pattern_image(64, 64), transform_policy(state.range(1))
    );
    const auto count = static_cast<std::size_t>(state.range(0));
    std::vector<ImageFingerprintRecord> records(count);
    std::vector<FingerprintId> ids(count);

    for (std::size_t i = 0; i < count; ++i) {
        auto fingerprint = query.variants.front();
        fingerprint.hash.words[0] ^= std::uint64_t{1} << (i % 64);
        fingerprint.quality = PdqQuality{i % 10 == 0 ? 49 : 100};
        ids[i] = FingerprintId{i};
        records[i] = {ids[i], MediaId{1 + i / 4}, fingerprint};
    }

    const ImageFingerprintCatalog catalog(records);
    constexpr PdqMatchPolicy policy{PdqDistance{31}, PdqQuality{50}};
    const auto expected = verify_image_candidates(MediaId{}, query, ids, catalog, policy);

    if (expected.size() != (count + 3) / 4) {
        state.SkipWithError("Image verification lost an eligible media match");

        return;
    }

    for (auto _ : state) {
        auto matches = verify_image_candidates(MediaId{0}, query, ids, catalog, policy);
        benchmark::DoNotOptimize(matches);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["matches"] = static_cast<double>(expected.size());
}

BENCHMARK(ImageVerification)
    ->Args({1000, 1})
    ->Args({1000, 8})
    ->Args({100000, 1})
    ->Args({100000, 8})
    ->ArgNames({"N", "variants"});

} // namespace
