#include <GhidraEngine/hash/blake3.hpp>

#include <blake3.h>

#include <array>
#include <fstream>
#include <istream>
#include <system_error>

namespace GhidraEngine {

namespace {

static_assert(BLAKE3_OUT_LEN == 32);

void check_cancelled(const std::stop_token &stop) {
    if (stop.stop_requested()) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_canceled),
            "BLAKE3 hashing cancelled"
        );
    }
}

Blake3Digest finalize(const blake3_hasher &hasher) noexcept {
    Blake3Digest result;
    blake3_hasher_finalize(&hasher, result.bytes.data(), result.bytes.size());

    return result;
}

}

Blake3Digest hash_blake3(std::span<const std::byte> bytes) noexcept {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    if (!bytes.empty()) {
        blake3_hasher_update(&hasher, bytes.data(), bytes.size());
    }

    return finalize(hasher);
}

Blake3Digest hash_blake3_stream(std::istream &input, const std::stop_token &stop) {
    check_cancelled(stop);

    if (!input.good()) {
        throw std::ios_base::failure("BLAKE3 requires a good input stream");
    }

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::array<char, 65536> buffer;

    for (;;) {
        check_cancelled(stop);

        try {
            input.read(buffer.data(), buffer.size());
        } catch (...) {
            if (input.bad() || !input.eof()) {
                throw std::ios_base::failure("BLAKE3 stream read failed");
            }
        }

        if (input.bad() || (input.fail() && !input.eof())) {
            throw std::ios_base::failure("BLAKE3 stream read failed");
        }

        check_cancelled(stop);
        blake3_hasher_update(&hasher, buffer.data(), static_cast<std::size_t>(input.gcount()));

        if (input.eof()) {
            break;
        }
    }

    check_cancelled(stop);

    return finalize(hasher);
}

Blake3Digest hash_blake3_file(const std::filesystem::path &path, const std::stop_token &stop) {
    check_cancelled(stop);

    if (path.empty() || path.native().find(std::filesystem::path::value_type{}) !=
                            std::filesystem::path::string_type::npos) {
        throw std::filesystem::filesystem_error(
            "Invalid BLAKE3 input path",
            path,
            std::make_error_code(std::errc::invalid_argument)
        );
    }

    std::error_code error;
    const auto status = std::filesystem::status(path, error);

    if (error) {
        throw std::filesystem::filesystem_error("Cannot inspect BLAKE3 input file", path, error);
    }

    if (!std::filesystem::is_regular_file(status)) {
        throw std::filesystem::filesystem_error(
            "BLAKE3 input must be a regular file",
            path,
            std::make_error_code(std::errc::invalid_argument)
        );
    }

    check_cancelled(stop);
    std::ifstream input(path, std::ios::binary);

    if (!input.is_open()) {
        throw std::filesystem::filesystem_error(
            "Cannot open BLAKE3 input file",
            path,
            std::make_error_code(std::errc::io_error)
        );
    }

    try {
        return hash_blake3_stream(input, stop);
    } catch (const std::ios_base::failure &failure) {
        throw std::filesystem::filesystem_error(
            "Cannot read BLAKE3 input file",
            path,
            failure.code()
        );
    }
}

}
