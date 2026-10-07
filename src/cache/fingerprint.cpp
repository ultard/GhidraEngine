#include <GhidraEngine/cache/fingerprint.hpp>

#include <GhidraEngine/hash/blake3.hpp>

#include "../decode_options.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace GhidraEngine {

namespace {

#ifdef _WIN32

std::mutex &cache_mutex() {
    static std::mutex mutex;

    return mutex;
}
#endif
std::string hex(const Blake3Digest &digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);

    for (const auto byte : digest.bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }

    return result;
}

std::filesystem::path entry_path(
    const std::filesystem::path &directory,
    const Blake3Digest &content,
    const Blake3Digest &settings
) {
    return directory / (hex(content) + "-" + hex(settings) + ".ghfp");
}

void append(std::vector<std::byte> &bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::byte>((value >> (i * 8U)) & 255U));
    }
}

template <class Options>
void append_decode(std::vector<std::byte> &bytes, const Options &options) {
    detail::check_decode_options(options);
    append(bytes, options.max_dimension);
    append(bytes, options.max_pixels);

    for (const auto value : options.alpha_background) {
        append(bytes, value);
    }
    append(bytes, std::bit_cast<std::uint64_t>(options.hdr.exposure));
    append(bytes, std::bit_cast<std::uint64_t>(options.hdr.peak_nits));
}

struct StagingDirectory {
    std::filesystem::path path;
    std::filesystem::path entry;

    explicit StagingDirectory(const std::filesystem::path &parent) {
        std::random_device random;

        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            auto candidate =
                parent / (".tmp-" + std::to_string(random()) + "-" + std::to_string(random()));
            auto candidate_entry = candidate / "entry";

            path = std::move(candidate);
            entry = std::move(candidate_entry);

            if (std::filesystem::create_directory(path)) {
                return;
            }
        }

        throw std::runtime_error("Cannot create exclusive cache staging directory");
    }

    StagingDirectory(const StagingDirectory &) = delete;

    StagingDirectory &operator=(const StagingDirectory &) = delete;

    ~StagingDirectory() noexcept {
        std::error_code ignored;
        std::filesystem::remove(entry, ignored);
        std::filesystem::remove(path, ignored);
    }
};
}

Blake3Digest fingerprint_settings_key(const FingerprintSettings &settings) {
    if (settings.kinds != MediaKinds::Images && settings.kinds != MediaKinds::Videos &&
        settings.kinds != MediaKinds::Both) {
        throw std::invalid_argument("Unknown media kinds");
    }

    if (settings.transforms != TransformPolicy::OriginalOnly &&
        settings.transforms != TransformPolicy::Rotations &&
        settings.transforms != TransformPolicy::Dihedral) {
        throw std::invalid_argument("Unknown image transform policy");
    }

    if (settings.video.max_frames == 0) {
        throw std::invalid_argument("Video frame budget must be positive");
    }

    constexpr std::string_view revision =
        "GhidraEngine-generation-2;hdr-reinhard-203;"
        "pdq-ec3671b;vips-8.16.0;ffmpeg-8.0;blake3-1.8.5";
    const auto revision_bytes = std::as_bytes(std::span(revision.data(), revision.size()));

    std::vector bytes(revision_bytes.begin(), revision_bytes.end());
    append(bytes, static_cast<std::uint8_t>(settings.kinds));
    append_decode(bytes, settings.image);

    append(bytes, static_cast<std::uint8_t>(settings.transforms));
    append_decode(bytes, settings.video.decode);

    append(bytes, static_cast<std::uint64_t>(settings.video.decode.sample_interval.count()));
    append(bytes, settings.video.prune_distance.has_value() ? 1 : 0);
    append(bytes, settings.video.prune_distance.value_or(PdqDistance{}).value());
    append(bytes, settings.video.max_frames);

    return hash_blake3(bytes);
}

FingerprintCache::FingerprintCache(const std::filesystem::path &directory) {
    if (directory.empty() ||
        directory.native().find(std::filesystem::path::value_type{}) !=
        std::filesystem::path::string_type::npos
    ) {
        throw std::invalid_argument("Invalid fingerprint cache directory");
    }

    directory_ = std::filesystem::absolute(directory);
    std::filesystem::create_directories(directory_);

    if (!std::filesystem::is_directory(directory_)) {
        throw std::invalid_argument("Fingerprint cache path must be a directory");
    }

    directory_ = std::filesystem::canonical(directory_);
}

std::optional<MediaFingerprint> FingerprintCache::load(
    const Blake3Digest &content,
    const Blake3Digest &settings,
    const std::size_t max_frames
) const {
    const auto path = entry_path(directory_, content, settings);
#ifdef _WIN32
    std::unique_lock lock(cache_mutex());
#endif
    std::error_code error;
    const auto status = std::filesystem::status(path, error);

    if ((!error && !std::filesystem::exists(status)) ||
        error == std::errc::no_such_file_or_directory) {
        return std::nullopt;
    }

    if (error) {
        throw std::filesystem::filesystem_error("Cannot inspect fingerprint cache", path, error);
    }

    if (!std::filesystem::is_regular_file(status)) {
        throw std::filesystem::filesystem_error(
            "Fingerprint cache entry must be a regular file",
            path,
            std::make_error_code(std::errc::invalid_argument)
        );
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();

    if (!input || length < 0) {
        throw std::filesystem::filesystem_error(
            "Cannot open fingerprint cache",
            path,
            std::make_error_code(std::errc::io_error)
        );
    }

    const auto size = static_cast<std::uintmax_t>(length);
    input.seekg(0);

    constexpr auto limit = std::min(
        std::vector<std::byte>{}.max_size(),
        static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())
    ) - 64;

    const auto frames = std::min(static_cast<std::uintmax_t>(max_frames), (limit - 47) / 41);
    const auto budget = std::max(std::uintmax_t{311}, std::uintmax_t{47} + frames * 41);

    if (size < 64 || size - 64 > budget || size - 64 > limit) {
        throw std::filesystem::filesystem_error(
            "Fingerprint cache exceeds byte budget",
            path,
            std::make_error_code(std::errc::file_too_large)
        );
    }

    std::vector<std::byte> bytes(size);

    if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)) ||
        input.peek() != std::char_traits<char>::eof() || input.bad()) {
        throw std::filesystem::filesystem_error(
            "Cannot read complete fingerprint cache",
            path,
            std::make_error_code(std::errc::io_error)
        );
    }

    input.close();
#ifdef _WIN32
    lock.unlock();
#endif

    for (std::size_t i = 0; i < 32; ++i) {
        if (bytes[i] != static_cast<std::byte>(content.bytes[i]) ||
            bytes[i + 32] != static_cast<std::byte>(settings.bytes[i])) {
            throw std::filesystem::filesystem_error(
                "Fingerprint cache key mismatch",
                path,
                std::make_error_code(std::errc::invalid_argument)
            );
        }
    }

    try {
        return deserialize_fingerprint(std::span(bytes).subspan(64), max_frames);
    } catch (const std::invalid_argument &failure) {
        throw std::filesystem::filesystem_error(
            failure.what(),
            path,
            std::make_error_code(std::errc::invalid_argument)
        );
    }
}

void FingerprintCache::store(
    const Blake3Digest &content,
    const Blake3Digest &settings,
    const MediaFingerprint &fingerprint
) const {
    auto bytes = serialize_fingerprint(fingerprint);
    std::array<std::byte, 64> keys{};
    std::size_t offset = 0;

    for (const auto *digest : {&content, &settings}) {
        for (const auto byte : digest->bytes) {
            keys[offset++] = static_cast<std::byte>(byte);
        }
    }

    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("Fingerprint cache exceeds stream size");
    }

    const StagingDirectory staging(directory_);
    const auto &temporary = staging.entry;

    std::ofstream output;
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.open(temporary, std::ios::binary);

    output.write(
        reinterpret_cast<const char *>(keys.data()),
        keys.size()
    );

    output.write(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );

    output.close();
    const auto target = entry_path(directory_, content, settings);
#ifdef _WIN32
    const std::lock_guard lock(cache_mutex());

    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        throw std::filesystem::filesystem_error(
            "Cannot publish fingerprint cache",
            temporary,
            target,
            std::error_code(static_cast<int>(GetLastError()), std::system_category())
        );
    }
#else
    std::filesystem::rename(temporary, target);
#endif
}
}
