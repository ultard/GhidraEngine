// Persistent signature cache: a rescan of an unchanged library does zero decodes
// and zero content hashes.
//
// Key is the path, validated by (size, mtime) — both already in hand from the
// directory listing, so a hit costs zero syscalls. Keying by file identity would
// survive renames but force an open() per file, the very cost this avoids.
//
// kCacheSchemaVersion invalidates every row whenever a change would alter a stored
// hash; comparing hashes from different code is worse than recomputing them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "ghidraengine/error.hpp"
#include "ghidraengine/types.hpp"

struct sqlite3;

namespace ghidraengine {

inline constexpr int kCacheSchemaVersion = 1;

struct CacheKey {
    std::string path; // UTF-8
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
};

struct CacheEntry {
    CacheKey key;
    MediaKind media = MediaKind::Unknown;
    Signature signature;
};

class SignatureCache {
public:
    SignatureCache() = default;
    ~SignatureCache();

    SignatureCache(const SignatureCache&) = delete;
    SignatureCache& operator=(const SignatureCache&) = delete;

    // Failure is never fatal to a scan; the caller simply computes everything.
    Result<void> open(const std::filesystem::path& path);
    [[nodiscard]] bool is_open() const noexcept { return db_ != nullptr; }

    // Every row into memory: rows are a few hundred bytes, so one sequential read
    // beats a query per file by a wide margin.
    Result<void> load();

    // False on a miss or when size/mtime no longer match.
    bool lookup(const CacheKey& key, MediaKind& media, Signature& signature) const;

    // Buffers an entry for writing. Thread-safe.
    void store(CacheEntry entry);

    // One transaction, then prunes rows untouched for `prune_after_days` (0 = off).
    Result<void> flush(std::uint32_t prune_after_days);

    [[nodiscard]] std::size_t loaded_rows() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t pending_rows() const;

private:
    Result<void> apply_schema();
    Result<void> write_batch(std::span<const CacheEntry> batch);

    sqlite3* db_ = nullptr;

    std::unordered_map<std::string, CacheEntry> entries_; // by path

    mutable std::mutex pending_mutex_;
    std::vector<CacheEntry> pending_;
};

} // namespace ghidraengine
