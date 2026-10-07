#include <GhidraEngine/hash/blake3.hpp>
#include <GhidraEngine/hash/pdq.hpp>
#include <GhidraEngine/image/decode.hpp>
#include <GhidraEngine/video/decode.hpp>
#include <GhidraEngine/video/fingerprint.hpp>

#include "../tests/support/test_support.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

void Blake3Memory(benchmark::State &state) {
    const std::vector bytes(static_cast<std::size_t>(state.range(0)), std::byte{42});

    for (auto _ : state) {
        auto hash = hash_blake3(bytes);
        benchmark::DoNotOptimize(hash);
    }

    state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(Blake3Memory)
    ->Arg(1024)
    ->Arg(65536)
    ->Arg(16 * 1024 * 1024);

void Blake3FileWarmCache(benchmark::State &state) {
    const TempDirectory directory;
    const auto path = directory.path() / "input.bin";
    const std::string bytes(static_cast<std::size_t>(state.range(0)), 'x');

    write_file(path, bytes);
    const auto expected = hash_blake3(std::as_bytes(std::span(bytes)));

    if (hash_blake3_file(path) != expected) {
        state.SkipWithError("File digest differs from memory digest");

        return;
    }

    for (auto _ : state) {
        auto hash = hash_blake3_file(path);
        benchmark::DoNotOptimize(hash);
    }

    state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(Blake3FileWarmCache)
    ->Arg(1024 * 1024)
    ->Arg(16 * 1024 * 1024)
    ->UseRealTime();

void PdqLuma(benchmark::State &state) {
    const auto image = pattern_image(
        static_cast<std::size_t>(state.range(0)),
        static_cast<std::size_t>(state.range(1))
    );

    for (auto _ : state) {
        auto hash = compute_pdq(image.luma, image.width, image.height);
        benchmark::DoNotOptimize(hash);
    }

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(PdqLuma)
    ->Args({64, 64})
    ->Args({512, 512})
    ->Args({1920, 1080})
    ->ArgNames({"width", "height"});

void ImageDecodeWarmCache(benchmark::State &state, const char *name) {
    const auto path = fixture(std::string("image/") + name);
    (void)decode_image(path);

    for (auto _ : state) {
        auto image = decode_image(path);
        benchmark::DoNotOptimize(image);
    }

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_CAPTURE(ImageDecodeWarmCache, PNG, "bridge.png")->UseRealTime();
BENCHMARK_CAPTURE(ImageDecodeWarmCache, JPEG, "bridge.jpg")->UseRealTime();
BENCHMARK_CAPTURE(ImageDecodeWarmCache, JPEG_Exif, "orientation-6.jpg")->UseRealTime();

void VideoDecodeWarmCache(benchmark::State &state) {
    constexpr std::array names{"vfr-alpha.mkv", "sd.mkv", "hd.mkv"};
    const auto path = fixture("video") / names.at(static_cast<std::size_t>(state.range(0)));

    VideoDecodeOptions options;
    options.sample_interval = Timestamp{state.range(1)};

    std::int64_t frames = 0;
    {
        VideoDecoder decoder(path, options);
        while (decoder.next()) {
            ++frames;
        }
    }

    if (frames == 0) {
        state.SkipWithError("Video has no decoded frames");
        return;
    }

    for (auto _ : state) {
        VideoDecoder decoder(path, options);

        while (auto frame = decoder.next()) {
            benchmark::DoNotOptimize(frame);
        }
    }

    state.SetItemsProcessed(state.iterations() * frames);
    state.counters["frames"] = static_cast<double>(frames);
}

void VideoArguments(benchmark::Benchmark *bench) {
    for (int clip : {0, 1, 2}) {
        for (int interval : {0, 250000}) {
            bench->Args({clip, interval});
        }
    }
}

BENCHMARK(VideoDecodeWarmCache)
    ->Apply(VideoArguments)
    ->ArgNames({"clip", "interval_us"})
    ->UseRealTime();

void VideoFingerprintWarmCache(benchmark::State &state) {
    constexpr std::array names{"vfr-alpha.mkv", "sd.mkv", "hd.mkv"};
    const auto path = fixture("video") / names.at(static_cast<std::size_t>(state.range(0)));

    VpdqGenerationOptions options;
    options.decode.sample_interval = Timestamp{state.range(1)};
    const auto expected = fingerprint_video(path, options);

    if (expected.frames.empty()) {
        state.SkipWithError("Video has no fingerprint frames");
        return;
    }

    for (auto _ : state) {
        auto signature = fingerprint_video(path, options);
        benchmark::DoNotOptimize(signature);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["frames"] = static_cast<double>(expected.frames.size());
}

BENCHMARK(VideoFingerprintWarmCache)
    ->Apply(VideoArguments)
    ->ArgNames({"clip", "interval_us"})
    ->UseRealTime();

}
