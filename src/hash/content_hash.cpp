#include "hash/content_hash.hpp"

#include <algorithm>
#include <cstring>

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace ghidraengine {
namespace {

Hash128 to_hash128(const XXH128_hash_t& value) noexcept {
    return Hash128{value.low64, value.high64};
}

}

Hash128 hash_bytes(std::span<const std::uint8_t> data) noexcept {
    return to_hash128(XXH3_128bits(data.data(), data.size()));
}

Result<Hash128> hash_partial(const platform::File& file, std::uint64_t size,
                             std::span<std::uint8_t> scratch) {
    const std::size_t chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(size, kPartialHashChunk));
    if (scratch.size() < chunk * 2) {
        return Error{ErrorCode::InvalidArgument, "partial hash scratch buffer too small"};
    }

    auto head = file.read_at(0, scratch.subspan(0, chunk));
    if (!head) {
        return head.error();
    }

    std::size_t total = head.value();

    if (size > kPartialHashChunk) {
        const std::uint64_t tail_offset = size - chunk;
        auto tail = file.read_at(tail_offset, scratch.subspan(total, chunk));
        if (!tail) {
            return tail.error();
        }
        total += tail.value();
    }

    XXH3_state_t state;
    XXH3_128bits_reset(&state);
    XXH3_128bits_update(&state, &size, sizeof(size));
    XXH3_128bits_update(&state, scratch.data(), total);
    return to_hash128(XXH3_128bits_digest(&state));
}

Result<Hash128> hash_full(const platform::File& file, std::span<std::uint8_t> scratch,
                          std::uint64_t* bytes_read) {
    if (scratch.empty()) {
        return Error{ErrorCode::InvalidArgument, "empty hash scratch buffer"};
    }

    XXH3_state_t state;
    XXH3_128bits_reset(&state);

    std::uint64_t offset = 0;
    for (;;) {
        auto read = file.read_at(offset, scratch);
        if (!read) {
            return read.error();
        }
        if (read.value() == 0) {
            break;
        }
        XXH3_128bits_update(&state, scratch.data(), read.value());
        offset += read.value();
        if (read.value() < scratch.size()) {
            break;
        }
    }

    if (bytes_read != nullptr) {
        *bytes_read += offset;
    }
    return to_hash128(XXH3_128bits_digest(&state));
}

Result<bool> files_identical(const std::filesystem::path& a, const std::filesystem::path& b,
                             std::uint64_t size) {
    auto file_a = platform::File::open_read(a, true);
    if (!file_a) {
        return file_a.error();
    }
    auto file_b = platform::File::open_read(b, true);
    if (!file_b) {
        return file_b.error();
    }

    auto size_a = file_a->size();
    auto size_b = file_b->size();
    if (!size_a) {
        return size_a.error();
    }
    if (!size_b) {
        return size_b.error();
    }
    if (size_a.value() != size || size_b.value() != size) {
        return false;
    }

    constexpr std::size_t kBlock = 256 * 1024;
    std::vector<std::uint8_t> buffer(kBlock * 2);
    const std::span<std::uint8_t> block_a(buffer.data(), kBlock);
    const std::span<std::uint8_t> block_b(buffer.data() + kBlock, kBlock);

    std::uint64_t offset = 0;
    while (offset < size) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBlock, size - offset));

        auto read_a = file_a->read_at(offset, block_a.subspan(0, want));
        if (!read_a) {
            return read_a.error();
        }
        auto read_b = file_b->read_at(offset, block_b.subspan(0, want));
        if (!read_b) {
            return read_b.error();
        }
        if (read_a.value() != read_b.value()) {
            return false;
        }
        if (read_a.value() == 0) {
            break;
        }
        if (std::memcmp(block_a.data(), block_b.data(), read_a.value()) != 0) {
            return false;
        }
        offset += read_a.value();
    }

    return true;
}

}
