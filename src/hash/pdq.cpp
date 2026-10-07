#include <GhidraEngine/hash/pdq.hpp>

#include "../cpu/kernels.hpp"
#include "pdq_internal.hpp"

#include <pdq/cpp/hashing/pdqhashing.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace GhidraEngine {

void detail::compute_pdq_variants(
    std::span<const float> luma,
    const std::size_t width,
    const std::size_t height,
    std::span<PdqFingerprint> output,
    PdqWorkspace &workspace
) {
    if (output.size() != 1 && output.size() != 4 && output.size() != 8) {
        throw std::invalid_argument("PDQ variant count must be 1, 4, or 8");
    }

    constexpr auto max_dimension = static_cast<std::size_t>(std::numeric_limits<int>::max() - 127);
    constexpr auto max_pixels = static_cast<std::size_t>(std::numeric_limits<int>::max());

    constexpr auto dimension_error = "PDQ dimensions must be positive, int-safe, and match luma";

    if (width == 0 || height == 0 || width > max_dimension || height > max_dimension) {
        throw std::invalid_argument(dimension_error);
    }

    if (width > max_pixels / height || luma.size() != width * height) {
        throw std::invalid_argument(dimension_error);
    }

    const bool valid_luma = detail::cpu_kernels().valid_luma(luma);

    if (!valid_luma) {
        throw std::invalid_argument("PDQ luma must be finite and in [0, 255]");
    }

    if (width < 5 || height < 5) {
        std::ranges::fill(output, PdqFingerprint{});

        return;
    }

    workspace.input.assign(luma.begin(), luma.end());
    workspace.scratch.resize(width == 64 && height == 64 ? 0 : luma.size());

    float buffer64[64][64];
    float buffer16x64[16][64];
    float buffer16[16][16];
    float auxiliary[16][16];

    namespace reference = facebook::pdq::hashing;
    int quality = 0;

    reference::pdqFloat256FromFloatLuma(
        workspace.input.data(),
        workspace.scratch.data(),
        static_cast<int>(height),
        static_cast<int>(width),
        buffer64,
        buffer16x64,
        buffer16,
        quality
    );

    using Transform = void (*)(float[16][16], float[16][16]);
    constexpr std::array<Transform, 8> transforms{
        nullptr,
        reference::dct16OriginalToRotate90,
        reference::dct16OriginalToRotate180,
        reference::dct16OriginalToRotate270,
        reference::dct16OriginalToFlipX,
        reference::dct16OriginalToFlipY,
        reference::dct16OriginalToFlipPlus1,
        reference::dct16OriginalToFlipMinus1
    };

    for (std::size_t variant = 0; variant < output.size(); ++variant) {
        reference::Hash256 hash;

        if (variant == 0) {
            reference::pdqBuffer16x16ToBits(buffer16, &hash);
        } else {
            transforms[variant](buffer16, auxiliary);
            reference::pdqBuffer16x16ToBits(auxiliary, &hash);
        }

        PdqHash compact;

        for (std::size_t i = 0; i < 16; ++i) {
            compact.words[i / 4] |= static_cast<std::uint64_t>(hash.w[i]) << (16 * (i % 4));
        }

        output[variant] = {compact, PdqQuality{quality}};
    }
}

void detail::compute_pdq_variants(
    std::span<const float> luma,
    const std::size_t width,
    const std::size_t height,
    std::span<PdqFingerprint> output
) {
    PdqWorkspace workspace;
    compute_pdq_variants(luma, width, height, output, workspace);
}

PdqFingerprint compute_pdq(std::span<const float> luma, std::size_t width, std::size_t height) {
    PdqFingerprint result;
    detail::compute_pdq_variants(luma, width, height, std::span{&result, 1});

    return result;
}

std::string encode_pdq(const PdqHash &hash) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text(64, '0');

    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto bit = (63 - i) * 4;
        text[i] = digits[(hash.words[bit / 64] >> (bit % 64)) & 15U];
    }

    return text;
}

PdqHash decode_pdq(std::string_view text) {
    if (text.size() != 64) {
        throw std::invalid_argument("PDQ text must contain exactly 64 hexadecimal digits");
    }

    PdqHash hash;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char character = text[i];
        unsigned value = 0;

        if (character >= '0' && character <= '9') {
            value = static_cast<unsigned>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            value = static_cast<unsigned>(character - 'a' + 10);
        } else if (character >= 'A' && character <= 'F') {
            value = static_cast<unsigned>(character - 'A' + 10);
        } else {
            throw std::invalid_argument("Invalid PDQ hexadecimal digit");
        }

        auto &word = hash.words[3 - i / 16];
        word = (word << 4U) | value;
    }

    return hash;
}

}
