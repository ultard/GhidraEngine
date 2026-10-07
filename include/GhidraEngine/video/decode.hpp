#ifndef GHIDRAENGINE_VIDEO_DECODE_HPP
#define GHIDRAENGINE_VIDEO_DECODE_HPP

#include <GhidraEngine/core/media.hpp>
#include <GhidraEngine/image/decode.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>

namespace GhidraEngine {

struct VideoDecodeOptions {
    std::size_t max_dimension = 32768;
    std::size_t max_pixels = 40'000'000;
    std::array<std::uint8_t, 3> alpha_background = {255, 255, 255};

    Timestamp sample_interval{};
    HdrToneMapOptions hdr;
};

struct DecodedVideoFrame {
    DecodedImage image;
    Timestamp timestamp{};
};

class GHIDRAENGINE_EXPORT VideoDecoder {
public:
    explicit VideoDecoder(
        const std::filesystem::path &path,
        const VideoDecodeOptions &options = {},
        const std::stop_token &stop = {}
    );

    ~VideoDecoder();

    VideoDecoder(VideoDecoder &&) noexcept;
    VideoDecoder &operator=(VideoDecoder &&) noexcept;

    VideoDecoder(const VideoDecoder &) = delete;

    VideoDecoder &operator=(const VideoDecoder &) = delete;

    [[nodiscard]] std::optional<DecodedVideoFrame> next() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
#endif
