#include <GhidraEngine/scan/scanner.hpp>

#include <array>
#include <cerrno>
#include <iostream>
#include <new>

struct AVCodecContext;
struct AVCodecParameters;

namespace {

int native_error = -ENOMEM;
std::stop_source *cancel_during_call = nullptr;

}

extern "C" int __wrap_avcodec_parameters_to_context(AVCodecContext *, const AVCodecParameters *) {
    if (cancel_during_call) {
        cancel_during_call->request_stop();
    }

    return native_error;

}

int main(int argc, char **argv) try {
    if (argc != 2) {
        return 2;
    }

    using namespace GhidraEngine;
    const auto path = std::filesystem::path(argv[1]);

    try {
        VideoDecoder decoder(path);

        return 1;
    } catch (const std::bad_alloc &) {
    }

    ScanOptions options;
    options.fingerprints.kinds = MediaKinds::Videos;
    const std::array roots{path};

    try {
        (void)scan_media(roots, options);

        return 1;
    } catch (const std::bad_alloc &) {
    }

    std::stop_source stop;
    cancel_during_call = &stop;

    try {
        VideoDecoder decoder(path, {}, stop.get_token());

        return 1;
    } catch (const std::system_error &error) {
        if (error.code() != std::errc::operation_canceled) {
            return 1;
        }
    }

    cancel_during_call = nullptr;
    native_error = -EINVAL;

    try {
        VideoDecoder decoder(path);

        return 1;
    } catch (const std::filesystem::filesystem_error &) {
    }

    const auto result = scan_media(roots, options);

    return result.files.size() == 1 && result.issues.size() == 1 ? 0 : 1;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
}
