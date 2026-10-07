#include <GhidraEngine/image/fingerprint.hpp>

#include "../hash/pdq_internal.hpp"

#include <cstddef>
#include <stdexcept>

namespace GhidraEngine {

ImageSignature fingerprint_image(const DecodedImage &image, TransformPolicy policy) {
    std::size_t count = 0;

    switch (policy) {
        case TransformPolicy::OriginalOnly:
            count = 1;
            break;
        case TransformPolicy::Rotations:
            count = 4;
            break;
        case TransformPolicy::Dihedral:
            count = 8;
            break;
        default:
            throw std::invalid_argument("Unknown image transform policy");
    }

    ImageSignature result{std::vector<PdqFingerprint>(count)};
    detail::compute_pdq_variants(image.luma, image.width, image.height, result.variants);

    return result;
}

}
