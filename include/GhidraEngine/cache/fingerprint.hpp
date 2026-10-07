#ifndef GHIDRAENGINE_CACHE_FINGERPRINT_HPP
#define GHIDRAENGINE_CACHE_FINGERPRINT_HPP

#include <GhidraEngine/io/fingerprint.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace GhidraEngine {

enum class MediaKinds : std::uint8_t {
    Images,
    Videos,
    Both
};

struct FingerprintSettings {
    MediaKinds kinds = MediaKinds::Both;
    ImageDecodeOptions image;
    TransformPolicy transforms = TransformPolicy::OriginalOnly;
    VpdqGenerationOptions video;
};

[[nodiscard]] GHIDRAENGINE_EXPORT Blake3Digest
fingerprint_settings_key(const FingerprintSettings &settings);

class GHIDRAENGINE_EXPORT FingerprintCache {
public:
    explicit FingerprintCache(const std::filesystem::path &directory);

    [[nodiscard]] std::optional<MediaFingerprint> load(
        const Blake3Digest &content,
        const Blake3Digest &settings,
        std::size_t max_frames = 1'000'000
    ) const;

    void store(
        const Blake3Digest &content,
        const Blake3Digest &settings,
        const MediaFingerprint &fingerprint
    ) const;

    [[nodiscard]] const std::filesystem::path &directory() const noexcept {
        return directory_;
    }

private:
    std::filesystem::path directory_;
};
}
#endif
