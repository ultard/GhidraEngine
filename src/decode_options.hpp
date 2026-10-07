#ifndef GHIDRAENGINE_DECODE_OPTIONS_HPP
#define GHIDRAENGINE_DECODE_OPTIONS_HPP

#include <GhidraEngine/video/decode.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace GhidraEngine::detail {

inline void check_hdr_options(const HdrToneMapOptions &options) {
    if (!std::isfinite(options.exposure) || options.exposure < 0.000001 ||
        options.exposure > 10000 ||
        !std::isfinite(options.peak_nits) || options.peak_nits < 100 || options.peak_nits > 10000) {
        throw std::invalid_argument(
            "HDR exposure must be in [0.000001, 10000], peak in [100, 10000] nits"
        );
    }
}

inline void check_decode_options(const ImageDecodeOptions &options) {
    check_hdr_options(options.hdr);

    constexpr auto int_limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
    constexpr auto error = "Image limits must be positive and fit PDQ/size arithmetic";

    if (options.max_dimension == 0 || options.max_dimension > int_limit / 8) {
        throw std::invalid_argument(error);
    }

    if (options.max_pixels == 0 || options.max_pixels > int_limit ||
        options.max_pixels > std::numeric_limits<std::size_t>::max() / (4 * sizeof(double))) {
        throw std::invalid_argument(error);
    }
}

inline void check_decode_options(const VideoDecodeOptions &options) {
    check_hdr_options(options.hdr);

    constexpr auto int_limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
    constexpr auto error = "Invalid video decode dimensions, pixel limit, or sample interval";

    if (options.max_dimension == 0 || options.max_dimension > int_limit / 8) {
        throw std::invalid_argument(error);
    }

    if (options.max_pixels == 0 || options.max_pixels > int_limit ||
        options.max_pixels > std::numeric_limits<std::size_t>::max() / 8) {
        throw std::invalid_argument(error);
    }

    if (options.sample_interval.count() < 0) {
        throw std::invalid_argument(error);
    }
}

}

#endif
