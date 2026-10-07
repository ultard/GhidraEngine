#include <GhidraEngine/io/fingerprint.hpp>

#include <GhidraEngine/hash/blake3.hpp>

#include <algorithm>
#include <bit>

namespace GhidraEngine {

namespace {

template <unsigned Width>
void append(std::vector<std::byte> &bytes, std::uint64_t value) {
    for (unsigned i = 0; i < Width; ++i) {
        bytes.push_back(static_cast<std::byte>((value >> (i * 8U)) & 255U));
    }
}

class Reader {
public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {
    }

    std::uint64_t read(const unsigned width) {
        if (bytes_.size() < width) {
            throw std::invalid_argument("Truncated fingerprint");
        }

        std::uint64_t value = 0;

        for (unsigned i = 0; i < width; ++i) {
            value |= std::to_integer<std::uint64_t>(bytes_[i]) << (i * 8U);
        }

        bytes_ = bytes_.subspan(width);

        return value;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size();
    }

    PdqFingerprint pdq() {
        PdqHash hash;

        for (auto &word : hash.words) {
            word = read(8);
        }

        return {hash, PdqQuality{static_cast<std::int64_t>(read(1))}};
    }

private:
    std::span<const std::byte> bytes_;
};

constexpr std::uint64_t magic = 0x50464847;

void append_pdq(std::vector<std::byte> &bytes, const PdqHash &hash, PdqQuality quality) {
    for (const auto word : hash.words) {
        append<8>(bytes, word);
    }

    append<1>(bytes, quality.value());
}
}

std::vector<std::byte> serialize_fingerprint(const MediaFingerprint &fingerprint) {
    const auto *image = std::get_if<ImageSignature>(&fingerprint);

    if (image) {
        detail::check_image_signature(*image);
    }

    const auto count =
        image ? image->variants.size() : std::get<VpdqSignature>(fingerprint).frames.size();

    const std::size_t width = image ? 33 : 41;
    constexpr std::size_t overhead = 47;

    std::vector<std::byte> bytes;

    if (count > (bytes.max_size() - overhead) / width) {
        throw std::length_error("Fingerprint exceeds byte capacity");
    }

    bytes.reserve(overhead + count * width);
    append<4>(bytes, magic);
    append<2>(bytes, 1);

    if (image) {
        append<1>(bytes, 1);
        append<8>(bytes, image->variants.size());

        for (const auto &variant : image->variants) {
            append_pdq(bytes, variant.hash, variant.quality);
        }
    } else {
        const auto &video = std::get<VpdqSignature>(fingerprint);
        append<1>(bytes, 2);
        append<8>(bytes, video.frames.size());

        for (const auto &frame : video.frames) {
            append_pdq(bytes, frame.hash, frame.quality);
            append<8>(bytes, std::bit_cast<std::uint64_t>(frame.timestamp.count()));
        }
    }

    const auto checksum = hash_blake3(bytes);

    for (const auto byte : checksum.bytes) {
        bytes.push_back(static_cast<std::byte>(byte));
    }

    return bytes;
}

MediaFingerprint deserialize_fingerprint(std::span<const std::byte> bytes, std::size_t max_frames) {
    if (bytes.size() < 47) {
        throw std::invalid_argument("Truncated fingerprint header/checksum");
    }

    const auto payload = bytes.first(bytes.size() - 32);
    const auto checksum = hash_blake3(payload);
    const bool valid_checksum = std::equal(
        checksum.bytes.begin(),
        checksum.bytes.end(),
        bytes.end() - 32,
        [](std::uint8_t a, std::byte b) {
            return a == std::to_integer<unsigned>(b);
        }
    );

    if (!valid_checksum) {
        throw std::invalid_argument("Fingerprint checksum mismatch");
    }

    Reader reader(payload);

    if (reader.read(4) != magic || reader.read(2) != 1) {
        throw std::invalid_argument("Unknown fingerprint format/version");
    }

    const auto kind = reader.read(1);
    const auto count = reader.read(8);

    const std::uint64_t width = kind == 1 ? 33 : 41;
    const auto invalid_count = [&] {
        if (kind == 1) {
            return count == 0 || count > 8;
        }

        if (kind == 2) {
            return count > max_frames;
        }

        return true;
    };

    if (invalid_count() || count != reader.remaining() / width || reader.remaining() % width != 0) {
        throw std::invalid_argument("Invalid fingerprint kind/count/length or frame budget");
    }

    if (kind == 1) {
        ImageSignature image;
        image.variants.reserve(count);

        for (std::uint64_t i = 0; i < count; ++i) {
            image.variants.push_back(reader.pdq());
        }

        return image;
    }

    VpdqSignature video;
    video.frames.reserve(count);

    for (std::uint64_t i = 0; i < count; ++i) {
        const auto pdq = reader.pdq();
        video.frames.push_back(
            {pdq.hash, pdq.quality, Timestamp{std::bit_cast<std::int64_t>(reader.read(8))}}
        );
    }

    return video;
}
}
