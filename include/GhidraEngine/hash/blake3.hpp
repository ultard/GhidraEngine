#ifndef GHIDRAENGINE_HASH_BLAKE3_HPP
#define GHIDRAENGINE_HASH_BLAKE3_HPP

#include <GhidraEngine/core/fingerprints.hpp>
#include <GhidraEngine/export.hpp>

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <stop_token>

namespace GhidraEngine {

[[nodiscard]] GHIDRAENGINE_EXPORT Blake3Digest
hash_blake3(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] GHIDRAENGINE_EXPORT Blake3Digest
hash_blake3_stream(std::istream &input, const std::stop_token &stop = {});

[[nodiscard]] GHIDRAENGINE_EXPORT Blake3Digest
hash_blake3_file(const std::filesystem::path &path, const std::stop_token &stop = {});

}

#endif
