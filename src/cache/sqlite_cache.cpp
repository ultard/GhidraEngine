#include "cache/sqlite_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <sqlite3.h>

#include "core/platform.hpp"
#include "hash/content_hash.hpp"

namespace ghidraengine {
namespace {

struct StoredSignature {
    std::uint64_t partial_low;
    std::uint64_t partial_high;
    std::uint64_t full_low;
    std::uint64_t full_high;
    std::uint64_t phash64;
    std::uint64_t phash256[4];
    std::uint64_t dhash64;
    std::uint8_t color[kColorMomentBytes];
    std::uint32_t image_width;
    std::uint32_t image_height;
    std::int64_t video_duration_ms;
    std::uint32_t video_width;
    std::uint32_t video_height;
    std::uint32_t video_frame_count;
    std::uint64_t video_frames[kMaxVideoFrames];
    std::uint8_t flags;
};

constexpr std::uint8_t kFlagPartial = 1U << 0;
constexpr std::uint8_t kFlagFull = 1U << 1;
constexpr std::uint8_t kFlagImage = 1U << 2;
constexpr std::uint8_t kFlagVideo = 1U << 3;
constexpr std::uint8_t kFlagColor = 1U << 4;

StoredSignature pack(const Signature& signature) {
    StoredSignature stored{};
    stored.partial_low = signature.partial_hash.low;
    stored.partial_high = signature.partial_hash.high;
    stored.full_low = signature.full_hash.low;
    stored.full_high = signature.full_hash.high;

    stored.phash64 = signature.image.phash64;
    std::memcpy(stored.phash256, signature.image.phash256.data(), sizeof(stored.phash256));
    stored.dhash64 = signature.image.dhash64;
    std::memcpy(stored.color, signature.image.color.data(), sizeof(stored.color));
    stored.image_width = signature.image.width;
    stored.image_height = signature.image.height;

    stored.video_duration_ms = signature.video.duration_ms;
    stored.video_width = signature.video.width;
    stored.video_height = signature.video.height;
    stored.video_frame_count = signature.video.frame_count;
    std::memcpy(stored.video_frames, signature.video.frames.data(), sizeof(stored.video_frames));

    stored.flags = static_cast<std::uint8_t>(
        (signature.has_partial_hash ? kFlagPartial : 0) |
        (signature.has_full_hash ? kFlagFull : 0) | (signature.has_image ? kFlagImage : 0) |
        (signature.has_video ? kFlagVideo : 0) |
        (signature.image.has_color ? kFlagColor : 0));
    return stored;
}

Signature unpack(const StoredSignature& stored) {
    Signature signature;
    signature.partial_hash = Hash128{stored.partial_low, stored.partial_high};
    signature.full_hash = Hash128{stored.full_low, stored.full_high};

    signature.image.phash64 = stored.phash64;
    std::memcpy(signature.image.phash256.data(), stored.phash256, sizeof(stored.phash256));
    signature.image.dhash64 = stored.dhash64;
    std::memcpy(signature.image.color.data(), stored.color, sizeof(stored.color));
    signature.image.width = stored.image_width;
    signature.image.height = stored.image_height;
    signature.image.has_color = (stored.flags & kFlagColor) != 0;

    signature.video.duration_ms = stored.video_duration_ms;
    signature.video.width = stored.video_width;
    signature.video.height = stored.video_height;
    signature.video.frame_count =
        std::min<std::uint32_t>(stored.video_frame_count, kMaxVideoFrames);
    std::memcpy(signature.video.frames.data(), stored.video_frames, sizeof(stored.video_frames));

    const std::size_t count = signature.video.frame_count;
    std::copy_n(signature.video.frames.begin(), count, signature.video.sorted.begin());
    std::sort(signature.video.sorted.begin(),
              signature.video.sorted.begin() + static_cast<std::ptrdiff_t>(count));

    signature.has_partial_hash = (stored.flags & kFlagPartial) != 0;
    signature.has_full_hash = (stored.flags & kFlagFull) != 0;
    signature.has_image = (stored.flags & kFlagImage) != 0;
    signature.has_video = (stored.flags & kFlagVideo) != 0;
    return signature;
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Error sqlite_error(sqlite3* db, std::string_view context) {
    const char* message = db != nullptr ? sqlite3_errmsg(db) : "no database";
    return Error{ErrorCode::CacheError, std::string(context) + ": " + message};
}

class Statement {
public:
    Statement() = default;
    ~Statement() { sqlite3_finalize(handle_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Result<void> prepare(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &handle_, nullptr) != SQLITE_OK) {
            return sqlite_error(db, std::string("prepare: ") + sql);
        }
        return {};
    }

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return handle_; }

private:
    sqlite3_stmt* handle_ = nullptr;
};

}

std::uint64_t signature_config_hash(const ScanConfig& config) noexcept {
    struct Fields {
        std::uint8_t detect_exact;
        std::uint8_t detect_similar;
        std::uint8_t dihedral_invariant;
        std::uint8_t padding0;
        std::uint32_t min_dimension;
        std::uint32_t frame_samples;
        std::uint32_t padding1;
        double edge_skip_fraction;
        double min_frame_variance;
    };
    static_assert(sizeof(Fields) == 32, "Fields must have no implicit padding");

    const Fields fields{
        static_cast<std::uint8_t>(config.detect_exact ? 1 : 0),
        static_cast<std::uint8_t>(config.detect_similar ? 1 : 0),
        static_cast<std::uint8_t>(config.image.dihedral_invariant ? 1 : 0),
        0,
        config.image.min_dimension,
        config.video.frame_samples,
        0,
        config.video.edge_skip_fraction,
        config.video.min_frame_variance,
    };

    return hash_bytes(std::span<const std::uint8_t>(
                          reinterpret_cast<const std::uint8_t*>(&fields), sizeof(fields)))
        .low;
}

SignatureCache::~SignatureCache() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

Result<void> SignatureCache::open(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const std::string utf8 = platform::to_utf8(path);
    if (sqlite3_open_v2(utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        Error error = sqlite_error(db_, "open cache");
        sqlite3_close(db_);
        db_ = nullptr;
        return error;
    }

    const char* pragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA cache_size=-16384;";
    sqlite3_exec(db_, pragmas, nullptr, nullptr, nullptr);

    return apply_schema();
}

Result<void> SignatureCache::apply_schema() {
    const auto exec = [&](const char* sql, std::string_view what) -> Result<void> {
        char* message = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) != SQLITE_OK) {
            Error error{ErrorCode::CacheError,
                        std::string(what) + ": " + (message != nullptr ? message : "")};
            sqlite3_free(message);
            return error;
        }
        return {};
    };

    if (auto created = exec("CREATE TABLE IF NOT EXISTS meta ("
                            "  key TEXT PRIMARY KEY,"
                            "  value INTEGER NOT NULL);",
                            "create meta table");
        !created) {
        return created;
    }

    int stored_version = 0;
    {
        Statement select;
        if (auto prepared = select.prepare(db_, "SELECT value FROM meta WHERE key='version';");
            !prepared) {
            return prepared;
        }
        if (sqlite3_step(select.get()) == SQLITE_ROW) {
            stored_version = sqlite3_column_int(select.get(), 0);
        }
    }

    if (stored_version != kCacheSchemaVersion) {
        sqlite3_exec(db_, "DROP TABLE IF EXISTS signatures;", nullptr, nullptr, nullptr);
    }

    if (auto created = exec("CREATE TABLE IF NOT EXISTS signatures ("
                            "  path TEXT PRIMARY KEY,"
                            "  size INTEGER NOT NULL,"
                            "  mtime_ns INTEGER NOT NULL,"
                            "  config_hash INTEGER NOT NULL,"
                            "  media INTEGER NOT NULL,"
                            "  seen_at INTEGER NOT NULL,"
                            "  payload BLOB NOT NULL) WITHOUT ROWID;",
                            "create signatures table");
        !created) {
        return created;
    }

    if (stored_version != kCacheSchemaVersion) {
        Statement update;
        if (auto prepared = update.prepare(
                db_, "INSERT INTO meta(key,value) VALUES('version',?1) "
                     "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
            !prepared) {
            return prepared;
        }
        sqlite3_bind_int(update.get(), 1, kCacheSchemaVersion);
        if (sqlite3_step(update.get()) != SQLITE_DONE) {
            return sqlite_error(db_, "record schema version");
        }
    }

    return {};
}

Result<void> SignatureCache::load() {
    if (db_ == nullptr) {
        return Error{ErrorCode::CacheError, "cache is not open"};
    }

    Statement select;
    if (auto prepared = select.prepare(
            db_, "SELECT path,size,mtime_ns,config_hash,media,payload FROM signatures;");
        !prepared) {
        return prepared;
    }

    while (sqlite3_step(select.get()) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 0));
        if (text == nullptr) {
            continue;
        }

        CacheEntry entry;
        entry.key.path.assign(text, static_cast<std::size_t>(
                                        sqlite3_column_bytes(select.get(), 0)));
        entry.key.size = static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 1));
        entry.key.mtime_ns = sqlite3_column_int64(select.get(), 2);
        entry.key.config_hash =
            static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 3));
        entry.media = static_cast<MediaKind>(sqlite3_column_int(select.get(), 4));

        const void* blob = sqlite3_column_blob(select.get(), 5);
        const int bytes = sqlite3_column_bytes(select.get(), 5);
        if (blob == nullptr || bytes != static_cast<int>(sizeof(StoredSignature))) {
            continue;
        }

        StoredSignature stored{};
        std::memcpy(&stored, blob, sizeof(stored));
        entry.signature = unpack(stored);

        std::string path = entry.key.path;
        entries_.emplace(std::move(path), std::move(entry));
    }

    return {};
}

bool SignatureCache::lookup(const CacheKey& key, MediaKind& media,
                            Signature& signature) const {
    const auto it = entries_.find(key.path);
    if (it == entries_.end()) {
        return false;
    }
    if (it->second.key.size != key.size || it->second.key.mtime_ns != key.mtime_ns ||
        it->second.key.config_hash != key.config_hash) {
        return false;
    }
    media = it->second.media;
    signature = it->second.signature;
    return true;
}

void SignatureCache::store(CacheEntry entry) {
    if (db_ == nullptr || entry.key.path.empty()) {
        return;
    }

    std::vector<CacheEntry> batch;
    {
        const std::lock_guard lock(pending_mutex_);
        pending_.push_back(std::move(entry));
        if (pending_.size() < kBatchRows) {
            return;
        }
        batch.swap(pending_);
    }

    (void)commit(std::move(batch), 0);
}

std::size_t SignatureCache::pending_rows() const {
    const std::lock_guard lock(pending_mutex_);
    return pending_.size();
}

Result<void> SignatureCache::write_batch(std::span<const CacheEntry> batch) {
    Statement insert;
    if (auto prepared = insert.prepare(
            db_,
            "INSERT INTO signatures(path,size,mtime_ns,config_hash,media,seen_at,payload)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7)"
            " ON CONFLICT(path) DO UPDATE SET"
            "  size=excluded.size, mtime_ns=excluded.mtime_ns,"
            "  config_hash=excluded.config_hash, media=excluded.media,"
            "  seen_at=excluded.seen_at, payload=excluded.payload;");
        !prepared) {
        return prepared;
    }

    const std::int64_t now = now_seconds();

    for (const CacheEntry& entry : batch) {
        const StoredSignature stored = pack(entry.signature);

        sqlite3_bind_text(insert.get(), 1, entry.key.path.data(),
                          static_cast<int>(entry.key.path.size()), SQLITE_STATIC);
        sqlite3_bind_int64(insert.get(), 2, static_cast<sqlite3_int64>(entry.key.size));
        sqlite3_bind_int64(insert.get(), 3, entry.key.mtime_ns);
        sqlite3_bind_int64(insert.get(), 4,
                           static_cast<sqlite3_int64>(entry.key.config_hash));
        sqlite3_bind_int(insert.get(), 5, static_cast<int>(entry.media));
        sqlite3_bind_int64(insert.get(), 6, now);
        sqlite3_bind_blob(insert.get(), 7, &stored, static_cast<int>(sizeof(stored)),
                          SQLITE_TRANSIENT);

        if (sqlite3_step(insert.get()) != SQLITE_DONE) {
            return sqlite_error(db_, "insert signature");
        }
        sqlite3_reset(insert.get());
    }

    return {};
}

Result<void> SignatureCache::flush(std::uint32_t prune_after_days) {
    if (db_ == nullptr) {
        return {};
    }

    std::vector<CacheEntry> batch;
    {
        const std::lock_guard lock(pending_mutex_);
        batch.swap(pending_);
    }

    return commit(std::move(batch), prune_after_days);
}

Result<void> SignatureCache::commit(std::vector<CacheEntry> batch,
                                    std::uint32_t prune_after_days) {
    if (batch.empty() && prune_after_days == 0) {
        return {};
    }

    const std::lock_guard write_lock(write_mutex_);

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return sqlite_error(db_, "begin transaction");
    }

    if (auto written = write_batch(batch); !written) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return written;
    }

    if (prune_after_days > 0) {
        Statement prune;
        if (auto prepared = prune.prepare(db_, "DELETE FROM signatures WHERE seen_at < ?1;");
            prepared) {
            const std::int64_t cutoff =
                now_seconds() - static_cast<std::int64_t>(prune_after_days) * 86400;
            sqlite3_bind_int64(prune.get(), 1, cutoff);
            sqlite3_step(prune.get());
        }
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return sqlite_error(db_, "commit transaction");
    }

    return {};
}

}
