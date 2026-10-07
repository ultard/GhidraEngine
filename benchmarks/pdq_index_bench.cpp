#include <GhidraEngine/index/flat_pdq.hpp>
#include <GhidraEngine/index/mih_pdq.hpp>

#include "../tests/support/test_support.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <random>
#include <type_traits>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

std::vector<PdqIndexEntry> corpus(std::size_t count, bool clustered) {
    std::mt19937_64 random(0x62656e6368);
    std::vector<PdqIndexEntry> entries(count);

    for (std::size_t i = 0; i < count; ++i) {
        PdqHash hash = random_hash(random);

        if (clustered) {
            hash = {};

            for (unsigned flip = 0; flip < 16; ++flip) {
                const auto bit = random() % 256;
                hash.words[bit / 64] ^= std::uint64_t{1} << (bit % 64);
            }
        }

        entries[i] = {FingerprintId{i}, hash};
    }

    entries[0].hash = {};
    entries[1].hash = {{0x7fff, 0, 0, 0}};
    entries[2].hash = {{0x7fffffff, 0, 0, 0}};

    return entries;
}

template <class Index>
void Search(benchmark::State &state) {
    const auto entries = corpus(static_cast<std::size_t>(state.range(0)), state.range(2) != 0);
    const Index index(entries);

    const auto radius = static_cast<std::uint16_t>(state.range(1));
    const auto expected = FlatPdqIndex(entries).search_within({}, radius);

    if (index.search_within({}, radius) != expected) {
        state.SkipWithError("Index result differs from Flat");

        return;
    }

    for (auto _ : state) {
        auto hits = index.search_within({}, radius);
        benchmark::DoNotOptimize(hits);
    }

    state.counters["hits"] = static_cast<double>(expected.size());
    state.SetItemsProcessed(state.iterations());

    if constexpr (std::is_same_v<Index, MihPdqIndex>) {
        MihSearchStats stats;
        (void)index.search_within({}, radius, &stats);
        state.counters["storage_bytes"] = static_cast<double>(index.storage_bytes());
        state.counters["verified"] = static_cast<double>(stats.candidates_verified);
        state.counters["posting_visits"] = static_cast<double>(stats.posting_visits);
        state.counters["slot_lookups"] = static_cast<double>(stats.slot_lookups);
        state.counters["used_flat"] = stats.used_flat ? 1 : 0;
    }
}

template <class Index>
void Build(benchmark::State &state) {
    auto entries = corpus(static_cast<std::size_t>(state.range(0)), false);
    std::mt19937_64 random(0x6275696c64);
    std::ranges::shuffle(entries, random);

    for (auto _ : state) {
        Index index(entries);
        benchmark::DoNotOptimize(index);
    }

    state.SetItemsProcessed(state.iterations() * state.range(0));
}

template <class Index>
void Insert(benchmark::State &state) {
    const auto entries = corpus(static_cast<std::size_t>(state.range(0)), false);

    for (auto _ : state) {
        Index index;
        index.reserve(entries.size());

        for (const auto &entry : entries) {
            index.insert(entry.id, entry.hash);
        }

        benchmark::DoNotOptimize(index);
    }

    state.SetItemsProcessed(state.iterations() * state.range(0));
}

void SearchArguments(benchmark::Benchmark *bench) {
    for (int count : {1000, 100000, 1000000}) {
        for (int radius : {0, 16, 31, 47, 48, 256}) {
            bench->Args({count, radius, 0});
        }

        bench->Args({count, 31, 1});
    }
}

BENCHMARK_TEMPLATE(Search, FlatPdqIndex)
    ->Apply(SearchArguments)
    ->ArgNames({"N", "radius", "clustered"});

BENCHMARK_TEMPLATE(Search, MihPdqIndex)
    ->Apply(SearchArguments)
    ->ArgNames({"N", "radius", "clustered"});

BENCHMARK_TEMPLATE(Build, FlatPdqIndex)
    ->Arg(1000)
    ->Arg(100000)
    ->Arg(1000000);

BENCHMARK_TEMPLATE(Build, MihPdqIndex)
    ->Arg(1000)
    ->Arg(100000)
    ->Arg(1000000);

BENCHMARK_TEMPLATE(Insert, FlatPdqIndex)
    ->Arg(1000)
    ->Arg(100000);

BENCHMARK_TEMPLATE(Insert, MihPdqIndex)
    ->Arg(1000)
    ->Arg(100000);

}
