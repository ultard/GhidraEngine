#ifndef GHIDRAENGINE_IO_FINGERPRINT_HPP
#define GHIDRAENGINE_IO_FINGERPRINT_HPP

#include <GhidraEngine/image/fingerprint.hpp>
#include <GhidraEngine/video/fingerprint.hpp>

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

namespace GhidraEngine {

using MediaFingerprint = std::variant<ImageSignature, VpdqSignature>;

[[nodiscard]] GHIDRAENGINE_EXPORT std::vector<std::byte>
serialize_fingerprint(const MediaFingerprint &fingerprint);

[[nodiscard]] GHIDRAENGINE_EXPORT MediaFingerprint
deserialize_fingerprint(std::span<const std::byte> bytes, std::size_t max_frames = 1'000'000);
}
#endif
