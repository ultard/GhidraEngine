#include "ghidraengine/types.hpp"

#include <functional>

namespace ghidraengine {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::NotFound: return "file not found";
        case ErrorCode::AccessDenied: return "access denied";
        case ErrorCode::IoError: return "I/O error";
        case ErrorCode::UnsupportedFormat: return "unsupported format";
        case ErrorCode::DecodeFailed: return "decode failed";
        case ErrorCode::CorruptFile: return "corrupt or truncated file";
        case ErrorCode::Cancelled: return "cancelled";
        case ErrorCode::CacheError: return "cache error";
        case ErrorCode::InvalidArgument: return "invalid argument";
        case ErrorCode::OutOfMemory: return "out of memory";
        case ErrorCode::Unknown: break;
    }
    return "unknown error";
}

const char* to_string(MediaKind kind) noexcept {
    switch (kind) {
        case MediaKind::Image: return "image";
        case MediaKind::Video: return "video";
        case MediaKind::Unknown: break;
    }
    return "unknown";
}

const char* to_string(MatchKind kind) noexcept {
    return kind == MatchKind::Exact ? "exact" : "similar";
}

const char* to_string(Progress::Phase phase) noexcept {
    switch (phase) {
        case Progress::Phase::Enumerating: return "enumerating";
        case Progress::Phase::Hashing: return "hashing";
        case Progress::Phase::Decoding: return "decoding";
        case Progress::Phase::Indexing: return "indexing";
        case Progress::Phase::Clustering: return "clustering";
        case Progress::Phase::Done: return "done";
    }
    return "unknown";
}

std::size_t FileIdentityHash::operator()(const FileIdentity& id) const noexcept {
    // Identities are already high-entropy, so a multiply-xor mix is plenty.
    std::uint64_t h = id.volume * 0x9E3779B97F4A7C15ULL;
    h ^= id.id_low + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= id.id_high + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
}

std::uint64_t Report::total_reclaimable_bytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& cluster : clusters) {
        total += cluster.reclaimable_bytes;
    }
    return total;
}

} // namespace ghidraengine
