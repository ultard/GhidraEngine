#include <GhidraEngine/video/fingerprint.hpp>

#include <GhidraEngine/hash/pdq.hpp>

#include "../hash/pdq_internal.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <system_error>

namespace GhidraEngine {

namespace {

bool retain(
    const VpdqSignature &signature,
    const PdqHash &hash,
    const std::optional<PdqDistance> distance
) {
    return !distance || signature.frames.empty() ||
           pdq_distance(signature.frames.back().hash, hash) > *distance;
}

void check_stop(const std::stop_token &stop) {
    if (stop.stop_requested()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
}

}

VpdqSignature fingerprint_video(
    const std::filesystem::path &path,
    const VpdqGenerationOptions &options,
    const std::stop_token &stop
) {
    if (options.max_frames == 0) {
        throw std::invalid_argument("vPDQ retained-frame budget must be positive");
    }

    check_stop(stop);
    VideoDecoder decoder(path, options.decode, stop);
    VpdqSignature signature;
    detail::PdqWorkspace workspace;

    while (auto frame = decoder.next()) {
        check_stop(stop);

        PdqFingerprint fingerprint;
        detail::compute_pdq_variants(
            frame->image.luma,
            frame->image.width,
            frame->image.height,
            std::span{&fingerprint, 1},
            workspace
        );

        check_stop(stop);

        if (!retain(signature, fingerprint.hash, options.prune_distance)) {
            continue;
        }

        if (signature.frames.size() == options.max_frames) {
            throw std::length_error("vPDQ retained-frame budget exceeded");
        }

        signature.frames.push_back({fingerprint.hash, fingerprint.quality, frame->timestamp});
    }

    check_stop(stop);

    return signature;
}

VpdqSignature prune_vpdq(const VpdqSignature &signature, PdqDistance max_distance) {
    VpdqSignature result;

    for (const auto &frame : signature.frames) {
        if (retain(result, frame.hash, max_distance)) {
            result.frames.push_back(frame);
        }
    }

    return result;
}

}
