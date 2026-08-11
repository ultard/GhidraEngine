// The two stages that dominate a scan: the JPEG decode fast path, which caps
// photo throughput, and MIH query latency, which decides whether the library
// scales past a few tens of thousands of files.
#include <random>
#include <vector>

#include <benchmark/benchmark.h>
#include <jpeglib.h>

#include "ghidraengine/ghidraengine.hpp"
#include "decode/image_decoder.hpp"
#include "hash/dct.hpp"
#include "hash/phash.hpp"
#include "index/mih_index.hpp"
#include "image_fixture.hpp"

using namespace ghidraengine;
using namespace ghidraengine::test;

namespace {

const std::vector<std::uint8_t>& jpeg_sample(std::uint32_t width, std::uint32_t height) {
    static std::vector<std::uint8_t> cached;
    static std::uint32_t cached_width = 0;
    static std::uint32_t cached_height = 0;
    if (cached_width != width || cached_height != height) {
        cached = encode_jpeg(make_image(width, height, 4242), 90);
        cached_width = width;
        cached_height = height;
    }
    return cached;
}

} // namespace

static void BM_Dct16Scalar(benchmark::State& state) {
    alignas(64) float input[kDctInputCount];
    alignas(64) float output[kDctOutputCount];
    std::mt19937 rng(1);
    for (float& value : input) {
        value = static_cast<float>(rng() % 256);
    }
    for (auto _ : state) {
        dct16_scalar(input, output);
        benchmark::DoNotOptimize(output);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dct16Scalar);

static void BM_Dct16Dispatched(benchmark::State& state) {
    alignas(64) float input[kDctInputCount];
    alignas(64) float output[kDctOutputCount];
    std::mt19937 rng(1);
    for (float& value : input) {
        value = static_cast<float>(rng() % 256);
    }
    const Dct16Fn kernel = dct16();
    for (auto _ : state) {
        kernel(input, output);
        benchmark::DoNotOptimize(output);
    }
    state.SetLabel(active_simd_backend());
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dct16Dispatched);

// How fast one photo becomes a comparable signature.
static void BM_DecodeJpeg(benchmark::State& state) {
    const auto width = static_cast<std::uint32_t>(state.range(0));
    const auto height = static_cast<std::uint32_t>(state.range(1));
    const std::vector<std::uint8_t>& data = jpeg_sample(width, height);

    for (auto _ : state) {
        auto thumb = decode_jpeg(data);
        benchmark::DoNotOptimize(thumb);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * data.size()));
}
BENCHMARK(BM_DecodeJpeg)
    ->Args({640, 480})
    ->Args({1920, 1080})
    ->Args({4000, 3000})
    ->Args({6000, 4000});

// The same file at full resolution, the way an ordinary image loader would do it.
// Both produce a hashable buffer; only one wastes 63/64ths of the work.
static void BM_DecodeJpegFullResolution(benchmark::State& state) {
    const auto width = static_cast<std::uint32_t>(state.range(0));
    const auto height = static_cast<std::uint32_t>(state.range(1));
    const std::vector<std::uint8_t>& data = jpeg_sample(width, height);

    for (auto _ : state) {
        jpeg_decompress_struct info{};
        jpeg_error_mgr error{};
        info.err = jpeg_std_error(&error);
        jpeg_create_decompress(&info);
        jpeg_mem_src(&info, data.data(), static_cast<unsigned long>(data.size()));
        jpeg_read_header(&info, TRUE);

        info.out_color_space = JCS_YCbCr; // same output space as the fast path
        info.do_fancy_upsampling = FALSE;
        info.do_block_smoothing = FALSE;
        info.dct_method = JDCT_ISLOW;
        jpeg_start_decompress(&info);

        const std::size_t stride =
            static_cast<std::size_t>(info.output_width) * info.output_components;
        std::vector<unsigned char> pixels(stride * info.output_height);
        while (info.output_scanline < info.output_height) {
            JSAMPROW row = pixels.data() + static_cast<std::size_t>(info.output_scanline) * stride;
            jpeg_read_scanlines(&info, &row, 1);
        }
        jpeg_finish_decompress(&info);
        jpeg_destroy_decompress(&info);
        benchmark::DoNotOptimize(pixels);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeJpegFullResolution)
    ->Args({640, 480})
    ->Args({1920, 1080})
    ->Args({4000, 3000})
    ->Args({6000, 4000});

static void BM_SignaturePipeline(benchmark::State& state) {
    const std::vector<std::uint8_t>& data = jpeg_sample(4000, 3000);
    const ImageMatchConfig config;

    for (auto _ : state) {
        auto thumb = decode_jpeg(data);
        if (thumb) {
            auto signature = compute_signature(*thumb, config);
            benchmark::DoNotOptimize(signature);
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SignaturePipeline);

static void BM_SignatureDihedral(benchmark::State& state) {
    const std::vector<std::uint8_t>& data = jpeg_sample(4000, 3000);
    ImageMatchConfig config;
    config.dihedral_invariant = true;

    auto thumb = decode_jpeg(data);
    if (!thumb) {
        state.SkipWithError("decode failed");
        return;
    }
    for (auto _ : state) {
        auto signature = compute_signature(*thumb, config);
        benchmark::DoNotOptimize(signature);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SignatureDihedral);

static void BM_MihBuild(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> codes(count);
    for (std::uint64_t& code : codes) {
        code = rng();
    }

    for (auto _ : state) {
        MihIndex index;
        index.build(codes);
        benchmark::DoNotOptimize(index);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * count));
}
BENCHMARK(BM_MihBuild)->Arg(10000)->Arg(100000)->Arg(1000000);

// The same query against a linear scan is below.
static void BM_MihQuery(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> codes(count);
    for (std::uint64_t& code : codes) {
        code = rng();
    }

    MihIndex index;
    index.build(codes);

    std::vector<std::uint32_t> candidates;
    std::vector<std::uint32_t> visited;
    std::uint32_t epoch = 0;
    std::size_t cursor = 0;

    for (auto _ : state) {
        index.query(codes[cursor++ % count], 10, candidates, visited, epoch);
        benchmark::DoNotOptimize(candidates);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MihQuery)->Arg(10000)->Arg(100000)->Arg(1000000);

static void BM_LinearScanQuery(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> codes(count);
    for (std::uint64_t& code : codes) {
        code = rng();
    }

    std::size_t cursor = 0;
    for (auto _ : state) {
        const std::uint64_t query = codes[cursor++ % count];
        std::uint32_t matches = 0;
        for (const std::uint64_t code : codes) {
            matches += (std::popcount(query ^ code) <= 10) ? 1 : 0;
        }
        benchmark::DoNotOptimize(matches);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LinearScanQuery)->Arg(10000)->Arg(100000)->Arg(1000000);

BENCHMARK_MAIN();
