#include "../src/cpu/kernels.hpp"
#include "../tests/support/test_support.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::detail;
using namespace GhidraEngine::test_support;

void Distance(benchmark::State &state) {
    const auto &kernels = *cpu_kernels_for(static_cast<CpuBackend>(state.range(0)));
    const auto threshold = static_cast<unsigned>(state.range(1));
    std::mt19937_64 random(0x73696d64);
    std::array<PdqHash, 1024> hashes;

    for (auto &hash : hashes) {
        hash = random_hash(random);
    }

    for (auto _ : state) {
        unsigned total = 0;

        for (const auto &hash : hashes) {
            total += kernels.bounded({}, hash, threshold);
        }

        benchmark::DoNotOptimize(total);
    }

    state.SetLabel(kernels.name);
    state.SetItemsProcessed(state.iterations() * hashes.size());
}

void LumaPixels(benchmark::State &state, bool opaque) {
    const auto &kernels = *cpu_kernels_for(static_cast<CpuBackend>(state.range(0)));
    const auto count = static_cast<std::size_t>(state.range(1));
    const auto bands = static_cast<unsigned>(state.range(2));
    const bool wide = state.range(3) != 0;
    std::vector<std::uint8_t> pixels8(count * bands, 127);
    std::vector<std::uint16_t> pixels16(count * bands, 127);
    std::vector<float> output(count);
    const std::array<std::uint8_t, 3> background{17, 128, 231};

    if (opaque && bands == 4) {
        for (std::size_t i = 0; i < count; ++i) {
            pixels8[i * bands + 3] = 255;
            pixels16[i * bands + 3] = 65535;
        }
    }

    for (auto _ : state) {
        if (wide) {
            kernels.luma16(pixels16.data(), output, bands, background);
        } else {
            kernels.luma8(pixels8.data(), output, bands, background);
        }

        benchmark::DoNotOptimize(output.data());
        benchmark::ClobberMemory();
    }

    state.SetLabel(kernels.name);
    state.SetItemsProcessed(state.iterations() * count);
}

void Luma(benchmark::State &state) {
    LumaPixels(state, false);
}

void LumaOpaque(benchmark::State &state) {
    LumaPixels(state, true);
}

void ValidateLuma(benchmark::State &state) {
    const auto &kernels = *cpu_kernels_for(static_cast<CpuBackend>(state.range(0)));
    const std::vector<float> pixels(static_cast<std::size_t>(state.range(1)), 127);

    for (auto _ : state) {
        auto valid = kernels.valid_luma(pixels);
        benchmark::DoNotOptimize(valid);
    }

    state.SetLabel(kernels.name);
    state.SetItemsProcessed(state.iterations() * pixels.size());
}

void DistanceArguments(benchmark::Benchmark *bench) {
    constexpr std::array backends{
        CpuBackend::Scalar,
        CpuBackend::Popcnt,
        CpuBackend::Avx2,
        CpuBackend::Neon
    };

    for (auto backend : backends) {
        if (!cpu_kernels_for(backend)) {
            continue;
        }

        for (int threshold : {0, 31, 128, 256}) {
            bench->Args({static_cast<int>(backend), threshold});
        }
    }
}

void LumaArguments(benchmark::Benchmark *bench) {
    for (auto backend : {CpuBackend::Scalar, CpuBackend::Avx2, CpuBackend::Neon}) {
        if (!cpu_kernels_for(backend)) {
            continue;
        }

        for (int count : {64, 1920}) {
            for (int bands : {3, 4}) {
                for (int wide : {0, 1}) {
                    bench->Args({static_cast<int>(backend), count, bands, wide});
                }
            }
        }
    }
}

void ValidationArguments(benchmark::Benchmark *bench) {
    for (auto backend : {CpuBackend::Scalar, CpuBackend::Avx2, CpuBackend::Neon}) {
        if (cpu_kernels_for(backend)) {
            bench->Args({static_cast<int>(backend), 256});
            bench->Args({static_cast<int>(backend), 1920 * 1080});
        }
    }
}

BENCHMARK(Distance)
    ->Apply(DistanceArguments)
    ->ArgNames({"backend", "radius"});

BENCHMARK(Luma)
    ->Apply(LumaArguments)
    ->ArgNames({"backend", "pixels", "bands", "wide"});

BENCHMARK(LumaOpaque)
    ->Apply(LumaArguments)
    ->ArgNames({"backend", "pixels", "bands", "wide"});

BENCHMARK(ValidateLuma)
    ->Apply(ValidationArguments)
    ->ArgNames({"backend", "pixels"});

}
