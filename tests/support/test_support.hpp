#pragma once

#include <GhidraEngine/core/media.hpp>
#include <GhidraEngine/image/decode.hpp>

#include "temp_directory.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string_view>

namespace GhidraEngine::test_support {

inline std::filesystem::path fixture(std::string_view relative) {
    return std::filesystem::path(GHIDRAENGINE_TEST_DATA_PATH) / relative;
}

inline void write_file(const std::filesystem::path &path, std::string_view bytes) {
    std::ofstream output;
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.open(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
}

inline PdqHash random_hash(std::mt19937_64 &random) {
    return {{random(), random(), random(), random()}};
}

// Deliberately bit-by-bit: independent of the production XOR/popcount implementation.
inline unsigned bit_distance(const PdqHash &a, const PdqHash &b) {
    unsigned distance = 0;

    for (unsigned bit = 0; bit < 256; ++bit) {
        distance +=
            ((a.words[bit / 64] >> (bit % 64)) & 1U) != ((b.words[bit / 64] >> (bit % 64)) & 1U);
    }

    return distance;
}

inline PdqHash prefix_hash(unsigned bits) {
    PdqHash hash;

    for (unsigned bit = 0; bit < bits; ++bit) {
        hash.words[bit / 64] |= std::uint64_t{1} << (bit % 64);
    }

    return hash;
}

inline DecodedImage pattern_image(std::size_t width, std::size_t height) {
    DecodedImage image{width, height, std::vector<float>(width * height)};

    for (std::size_t i = 0; i < image.luma.size(); ++i) {
        image.luma[i] = static_cast<float>((i * 37 + (i / width) * 19 + 11) % 251);
    }

    return image;
}

inline float luma(unsigned r, unsigned g, unsigned b) {
    return 0.299F * static_cast<float>(r) + 0.587F * static_cast<float>(g) +
           0.114F * static_cast<float>(b);
}

} // namespace GhidraEngine::test_support
