#ifndef GHIDRAENGINE_IMAGE_FINGERPRINT_HPP
#define GHIDRAENGINE_IMAGE_FINGERPRINT_HPP

#include <GhidraEngine/core/media.hpp>
#include <GhidraEngine/hash/pdq.hpp>
#include <GhidraEngine/image/decode.hpp>

#include <stdexcept>
#include <vector>

namespace GhidraEngine {

struct ImageSignature {
    std::vector<PdqFingerprint> variants;
    bool operator==(const ImageSignature &) const = default;
};

[[nodiscard]] GHIDRAENGINE_EXPORT ImageSignature fingerprint_image(
    const DecodedImage &image,
    TransformPolicy policy = TransformPolicy::OriginalOnly
);

namespace detail {

inline void check_image_signature(const ImageSignature &signature) {
    if (signature.variants.empty() || signature.variants.size() > 8) {
        throw std::invalid_argument("Image query must contain 1 to 8 PDQ variants");
    }
}
}
}
#endif
