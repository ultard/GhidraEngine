#include <GhidraEngine/cache/fingerprint.hpp>
#include <GhidraEngine/io/fingerprint.hpp>
#include <GhidraEngine/scan/scanner.hpp>

#include "../tests/support/test_support.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

void Scan(benchmark::State &state, bool cached) {
    TempDirectory directory;
    const auto media = directory.path() / "media";
    std::filesystem::create_directory(media);

    for (unsigned i = 0; i < 64; ++i) {
        const auto input = fixture(i % 2 == 0 ? "image/bridge.png" : "video/vfr-alpha.mkv");
        std::filesystem::copy_file(input, media / std::to_string(i));
    }

    const FingerprintCache cache(directory.path() / "cache");
    const std::array roots{media};

    ScanOptions options;
    options.workers = static_cast<std::size_t>(state.range(0));

    const auto *active_cache = cached ? &cache : nullptr;
    const auto initial = scan_media(roots, options, active_cache);

    if (!initial.issues.empty() || initial.files.size() != 64 ||
        !std::ranges::all_of(initial.files, [](const auto &file) {
            return file.digest.has_value() && file.fingerprint.has_value();
        })) {
        state.SkipWithError("Scan did not fingerprint every fixture");

        return;
    }

    if (cached) {
        const auto warm = scan_media(roots, options, active_cache);

        if (!warm.issues.empty() || warm.files.size() != 64 ||
            !std::ranges::all_of(warm.files, [](const auto &file) { return file.cache_hit; })) {
            state.SkipWithError("Warm scan did not hit the fingerprint cache for every file");

            return;
        }
    }

    for (auto _ : state) {
        auto result = scan_media(roots, options, active_cache);
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations() * 64);
}

BENCHMARK_CAPTURE(Scan, NoFingerprintCache, false)
    ->Arg(1)
    ->Arg(4)
    ->ArgName("workers")
    ->UseRealTime();

BENCHMARK_CAPTURE(Scan, WarmFingerprintCache, true)
    ->Arg(1)
    ->Arg(4)
    ->ArgName("workers")
    ->UseRealTime();

void Serialization(benchmark::State &state, bool encode) {
    std::mt19937_64 random(0x73657269616c);
    VpdqSignature video;

    for (std::int64_t i = 0; i < state.range(0); ++i) {
        video.frames.push_back({random_hash(random), PdqQuality{100}, Timestamp{i * 1000000}});
    }

    const MediaFingerprint fingerprint{std::move(video)};
    const auto bytes = serialize_fingerprint(fingerprint);

    if (deserialize_fingerprint(bytes) != fingerprint) {
        state.SkipWithError("Fingerprint serialization changed frame data");

        return;
    }

    for (auto _ : state) {
        if (encode) {
            auto output = serialize_fingerprint(fingerprint);
            benchmark::DoNotOptimize(output);
        } else {
            auto output = deserialize_fingerprint(bytes);
            benchmark::DoNotOptimize(output);
        }
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes.size()));
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_CAPTURE(Serialization, Encode, true)
    ->Arg(1000)
    ->Arg(100000)
    ->ArgName("frames");

BENCHMARK_CAPTURE(Serialization, Decode, false)
    ->Arg(1000)
    ->Arg(100000)
    ->ArgName("frames");

} // namespace
