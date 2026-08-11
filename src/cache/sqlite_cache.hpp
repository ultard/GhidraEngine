#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "ghidraengine/config.hpp"
#include "ghidraengine/error.hpp"
#include "ghidraengine/types.hpp"

struct sqlite3;

namespace ghidraengine {

inline constexpr int kCacheSchemaVersion = 2;

std::uint64_t signature_config_hash(const ScanConfig& config) noexcept;

struct CacheKey {
    std::string path;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint64_t config_hash = 0;
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

    Result<void> open(const std::filesystem::path& path);
    [[nodiscard]] bool is_open() const noexcept { return db_ != nullptr; }

    Result<void> load();

    bool lookup(const CacheKey& key, MediaKind& media, Signature& signature) const;

    void store(CacheEntry entry);

    Result<void> flush(std::uint32_t prune_after_days);

    [[nodiscard]] std::size_t loaded_rows() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t pending_rows() const;

private:
    Result<void> apply_schema();
    Result<void> write_batch(std::span<const CacheEntry> batch);
    Result<void> commit(std::vector<CacheEntry> batch, std::uint32_t prune_after_days);

    static constexpr std::size_t kBatchRows = 4096;

    sqlite3* db_ = nullptr;

    std::unordered_map<std::string, CacheEntry> entries_;

    mutable std::mutex pending_mutex_;
    std::vector<CacheEntry> pending_;

    std::mutex write_mutex_;
};

}
