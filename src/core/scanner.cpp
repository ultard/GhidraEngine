// Scan cascade: walk → cache lookup → first touch → exact grouping (size,
// head+tail hash, full hash) → perceptual grouping via MIH → clusters. Each stage
// only sees what survived the previous one.
#include "ghidraengine/ghidraengine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "cache/sqlite_cache.hpp"
#include "core/enumerate.hpp"
#include "core/file_io.hpp"
#include "core/platform.hpp"
#include "core/thread_pool.hpp"
#include "decode/image_decoder.hpp"
#include "hash/content_hash.hpp"
#include "hash/dct.hpp"
#include "hash/phash.hpp"
#include "index/cluster.hpp"
#include "index/mih_index.hpp"
#include "video/video_signature.hpp"

namespace ghidraengine {
namespace {

// Above this an image is not loaded whole; the decoder needs it all resident.
constexpr std::uint64_t kMaxImageBytes = 512ULL * 1024 * 1024;

// Grown once per worker so the steady state never allocates.
struct ThreadScratch {
    std::vector<std::uint8_t> file_buffer;
    std::vector<std::uint8_t> stream_buffer;
    std::vector<std::uint8_t> partial_buffer;
};

ThreadScratch& scratch() {
    thread_local ThreadScratch storage;
    return storage;
}

// Caps workers inside a read, independently of how many are decoding: honouring
// io_threads by shrinking the pool would idle cores during CPU-bound decode.
class IoLimiter {
public:
    explicit IoLimiter(unsigned permits) : available_(permits) {}

    void acquire() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return available_ > 0; });
        --available_;
    }

    void release() {
        {
            const std::lock_guard lock(mutex_);
            ++available_;
        }
        ready_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    unsigned available_;
};

class IoGuard {
public:
    explicit IoGuard(IoLimiter& limiter) : limiter_(&limiter) { limiter_->acquire(); }
    ~IoGuard() { release(); }

    IoGuard(const IoGuard&) = delete;
    IoGuard& operator=(const IoGuard&) = delete;

    // Hands the slot back before CPU work starts.
    void release() noexcept {
        if (limiter_ != nullptr) {
            limiter_->release();
            limiter_ = nullptr;
        }
    }

private:
    IoLimiter* limiter_;
};

struct FileWork {
    Signature signature;
    MediaKind media = MediaKind::Unknown;
    bool usable = false;    // survived filters and produced something comparable
    bool from_cache = false;
};

std::uint64_t pixel_count(const FileWork& work) {
    if (work.media == MediaKind::Image) {
        return static_cast<std::uint64_t>(work.signature.image.width) *
               work.signature.image.height;
    }
    if (work.media == MediaKind::Video) {
        return static_cast<std::uint64_t>(work.signature.video.width) *
               work.signature.video.height;
    }
    return 0;
}

} // namespace

struct Scanner::Impl {
    ScanConfig config;
    std::stop_source internal_stop;

    Result<Report> run(std::span<const std::filesystem::path> roots, std::stop_token token);

    void process_file(const FileEntry& entry, FileWork& work, SignatureCache* cache,
                      IoLimiter& io, std::vector<FileError>& errors, ScanStats& stats,
                      std::mutex& guard);

    std::vector<Cluster> find_exact_clusters(const std::vector<FileEntry>& files,
                                             std::vector<FileWork>& work,
                                             const std::vector<KeeperInfo>& keeper_info,
                                             ThreadPool& pool, const std::stop_token& token,
                                             std::vector<FileError>& errors);

    std::vector<Cluster> find_similar_images(const std::vector<FileEntry>& files,
                                             const std::vector<FileWork>& work,
                                             const std::vector<KeeperInfo>& keeper_info,
                                             ThreadPool& pool, const std::stop_token& token);

    std::vector<Cluster> find_similar_videos(const std::vector<FileEntry>& files,
                                             const std::vector<FileWork>& work,
                                             const std::vector<KeeperInfo>& keeper_info,
                                             const std::stop_token& token);

    Cluster finalize(std::vector<std::uint32_t> members, MatchKind kind, MediaKind media,
                     const std::vector<FileEntry>& files,
                     const std::vector<KeeperInfo>& keeper_info,
                     const std::function<std::uint32_t(std::uint32_t, std::uint32_t)>& distance);

    void report_progress(Progress::Phase phase, std::uint64_t processed, std::uint64_t total) {
        if (config.on_progress) {
            config.on_progress(Progress{phase, processed, total});
        }
    }
};

void Scanner::Impl::process_file(const FileEntry& entry, FileWork& work, SignatureCache* cache,
                                 IoLimiter& io, std::vector<FileError>& errors,
                                 ScanStats& stats, std::mutex& guard) {
    ThreadScratch& buffers = scratch();

    const auto fail = [&](Error error) {
        FileError failure{entry.path, std::move(error)};
        if (config.on_error) {
            config.on_error(failure);
        }
        const std::lock_guard lock(guard);
        errors.push_back(std::move(failure));
    };

    CacheKey key;
    key.path = platform::to_utf8(entry.path);
    key.size = entry.size;
    key.mtime_ns = entry.mtime_ns;

    if (cache != nullptr && cache->lookup(key, work.media, work.signature)) {
        const bool wanted = (work.media == MediaKind::Image && config.scan_images) ||
                            (work.media == MediaKind::Video && config.scan_videos);
        if (!wanted) {
            return;
        }
        work.usable = true;
        work.from_cache = true;
        const std::lock_guard lock(guard);
        ++stats.cache_hits;
        return;
    }

    // Held until the reads finish; decoding happens after it is given back so CPU
    // work is never throttled by the I/O budget.
    IoGuard io_guard(io);

    auto file = platform::File::open_read(entry.path, /*sequential=*/true);
    if (!file) {
        fail(file.error());
        return;
    }

    std::array<std::uint8_t, kHeaderBytes> header{};
    auto header_read = read_header(*file, std::span<std::uint8_t, kHeaderBytes>(header));
    if (!header_read) {
        fail(header_read.error());
        return;
    }

    work.media = classify_header(std::span<const std::uint8_t>(header.data(), header_read.value()));
    if (work.media == MediaKind::Image && !config.scan_images) {
        return;
    }
    if (work.media == MediaKind::Video && !config.scan_videos) {
        return;
    }
    if (work.media == MediaKind::Unknown) {
        return; // not media we can compare; skipped, not an error
    }

    bool produced_something = false;

    // One read serves both the hash and the decoder.
    if (work.media == MediaKind::Image) {
        if (entry.size <= kMaxImageBytes) {
            buffers.file_buffer.resize(static_cast<std::size_t>(entry.size));
            auto read = file->read_at(0, buffers.file_buffer);
            if (!read) {
                fail(read.error());
                return;
            }
            buffers.file_buffer.resize(read.value());

            io_guard.release(); // resident now; everything below is computation

            {
                const std::lock_guard lock(guard);
                stats.bytes_read += read.value();
            }

            if (config.detect_exact) {
                work.signature.full_hash = hash_bytes(buffers.file_buffer);
                work.signature.has_full_hash = true;
                work.signature.partial_hash = work.signature.full_hash;
                work.signature.has_partial_hash = true;
                produced_something = true;
                const std::lock_guard lock(guard);
                ++stats.files_hashed;
            }

            if (config.detect_similar) {
                auto thumb = decode_image(buffers.file_buffer);
                if (thumb) {
                    const std::uint32_t smallest =
                        std::min(thumb->source_width, thumb->source_height);
                    if (smallest >= config.image.min_dimension) {
                        work.signature.image = compute_signature(*thumb, config.image);
                        work.signature.has_image = true;
                        produced_something = true;
                    }
                    const std::lock_guard lock(guard);
                    ++stats.images_decoded;
                } else if (!config.detect_exact) {
                    fail(thumb.error()); // only a failure if nothing else was produced
                    return;
                }
            }
        } else if (config.detect_exact) {
            // Too large to hold; fall back to the streaming cascade.
            buffers.partial_buffer.resize(kPartialHashChunk * 2);
            auto partial = hash_partial(*file, entry.size, buffers.partial_buffer);
            if (!partial) {
                fail(partial.error());
                return;
            }
            work.signature.partial_hash = partial.value();
            work.signature.has_partial_hash = true;
            produced_something = true;
        }
    }

    // Videos are never read whole; the container is probed instead.
    if (work.media == MediaKind::Video) {
        if (config.detect_exact) {
            buffers.partial_buffer.resize(kPartialHashChunk * 2);
            auto partial = hash_partial(*file, entry.size, buffers.partial_buffer);
            if (!partial) {
                fail(partial.error());
                return;
            }
            work.signature.partial_hash = partial.value();
            work.signature.has_partial_hash = true;
            if (partial_hash_is_complete(entry.size)) {
                work.signature.full_hash = partial.value();
                work.signature.has_full_hash = true;
            }
            produced_something = true;

            const std::lock_guard lock(guard);
            stats.bytes_read += std::min<std::uint64_t>(entry.size, kPartialHashChunk * 2);
            ++stats.files_hashed;
        }

        if (config.detect_similar) {
            file->close(); // FFmpeg opens the path itself; don't hold two handles

            auto signature = extract_video_signature(entry.path, config.video);
            if (signature) {
                work.signature.video = signature.value();
                work.signature.has_video = true;
                produced_something = true;
            } else if (!config.detect_exact) {
                fail(signature.error());
                return;
            }
            const std::lock_guard lock(guard);
            ++stats.videos_probed;
        }
    }

    if (!produced_something) {
        return;
    }

    work.usable = true;

    if (cache != nullptr) {
        CacheEntry stored;
        stored.key = std::move(key);
        stored.media = work.media;
        stored.signature = work.signature;
        cache->store(std::move(stored));
    }
}

std::vector<Cluster> Scanner::Impl::find_exact_clusters(
    const std::vector<FileEntry>& files, std::vector<FileWork>& work,
    const std::vector<KeeperInfo>& keeper_info, ThreadPool& pool,
    const std::stop_token& token, std::vector<FileError>& errors) {
    std::vector<Cluster> clusters;

    // A unique size cannot be a byte-exact duplicate of anything, so the whole
    // hashing cascade is skipped for it.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_size;
    for (std::uint32_t i = 0; i < files.size(); ++i) {
        if (work[i].usable && work[i].signature.has_partial_hash) {
            by_size[files[i].size].push_back(i);
        }
    }

    // Within each size group, split by head+tail hash.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_partial;
    std::vector<std::vector<std::uint32_t>> candidates;

    for (auto& [size, group] : by_size) {
        if (group.size() < 2) {
            continue;
        }
        by_partial.clear();
        for (const std::uint32_t index : group) {
            by_partial[work[index].signature.partial_hash.low].push_back(index);
        }
        for (auto& [hash, bucket] : by_partial) {
            if (bucket.size() >= 2) {
                candidates.push_back(std::move(bucket));
            }
        }
    }

    // Only survivors the head+tail probe did not already cover pay for a full read.
    std::vector<std::uint32_t> need_full;
    for (const auto& group : candidates) {
        for (const std::uint32_t index : group) {
            if (!work[index].signature.has_full_hash) {
                need_full.push_back(index);
            }
        }
    }

    if (!need_full.empty()) {
        std::mutex guard;
        std::atomic<std::uint64_t> processed{0};

        parallel_for(
            pool, 0, need_full.size(),
            [&](std::size_t slot) {
                const std::uint32_t index = need_full[slot];
                ThreadScratch& buffers = scratch();
                buffers.stream_buffer.resize(kStreamBufferSize);

                auto file = platform::File::open_read(files[index].path, /*sequential=*/true);
                if (!file) {
                    const std::lock_guard lock(guard);
                    errors.push_back(FileError{files[index].path, file.error()});
                    return;
                }

                std::uint64_t bytes = 0;
                auto hash = hash_full(*file, buffers.stream_buffer, &bytes);
                if (!hash) {
                    const std::lock_guard lock(guard);
                    errors.push_back(FileError{files[index].path, hash.error()});
                    return;
                }
                work[index].signature.full_hash = hash.value();
                work[index].signature.has_full_hash = true;

                const std::uint64_t done = processed.fetch_add(1) + 1;
                if (done % 64 == 0) {
                    report_progress(Progress::Phase::Hashing, done, need_full.size());
                }
            },
            token);
    }

    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_full;
    for (const auto& group : candidates) {
        by_full.clear();
        for (const std::uint32_t index : group) {
            if (work[index].signature.has_full_hash) {
                by_full[work[index].signature.full_hash.low].push_back(index);
            }
        }

        for (auto& [hash, bucket] : by_full) {
            if (bucket.size() < 2) {
                continue;
            }

            // The map key is only 64 bits; confirm the full 128, then the bytes.
            std::vector<std::uint32_t> confirmed;
            confirmed.push_back(bucket.front());
            const Hash128& reference = work[bucket.front()].signature.full_hash;

            for (std::size_t i = 1; i < bucket.size(); ++i) {
                if (!(work[bucket[i]].signature.full_hash == reference)) {
                    continue;
                }
                if (config.verify_bytes) {
                    auto identical = files_identical(files[bucket.front()].path,
                                                     files[bucket[i]].path,
                                                     files[bucket[i]].size);
                    if (!identical || !identical.value()) {
                        continue;
                    }
                }
                confirmed.push_back(bucket[i]);
            }

            if (confirmed.size() < 2) {
                continue;
            }
            std::sort(confirmed.begin(), confirmed.end());
            clusters.push_back(finalize(std::move(confirmed), MatchKind::Exact,
                                        work[bucket.front()].media, files, keeper_info,
                                        [](std::uint32_t, std::uint32_t) { return 0U; }));
        }
    }

    return clusters;
}

std::vector<Cluster> Scanner::Impl::find_similar_images(
    const std::vector<FileEntry>& files, const std::vector<FileWork>& work,
    const std::vector<KeeperInfo>& keeper_info, ThreadPool& pool,
    const std::stop_token& token) {
    std::vector<std::uint32_t> subject;
    std::vector<std::uint64_t> codes;

    for (std::uint32_t i = 0; i < files.size(); ++i) {
        if (work[i].usable && work[i].signature.has_image) {
            subject.push_back(i);
            codes.push_back(work[i].signature.image.phash64);
        }
    }
    if (subject.size() < 2) {
        return {};
    }

    report_progress(Progress::Phase::Indexing, 0, subject.size());

    MihIndex index;
    index.build(codes);

    // Queries are read-only against the index and each worker owns its pair list,
    // so no lock appears in the inner loop.
    const std::size_t workers = pool.size() + 1;
    std::vector<std::vector<MatchPair>> per_worker(workers);
    std::atomic<std::uint64_t> queried{0};

    parallel_for(
        pool, 0, workers,
        [&](std::size_t slot) {
            std::vector<MatchPair>& pairs = per_worker[slot];
            std::vector<std::uint32_t> candidates;
            std::vector<std::uint32_t> visited;
            std::uint32_t epoch = 0;

            // Strided, not blocked: spreads a dense neighbourhood across workers.
            for (std::size_t local = slot; local < subject.size(); local += workers) {
                if (token.stop_possible() && token.stop_requested()) {
                    return;
                }

                index.query(codes[local], config.image.phash_threshold, candidates, visited,
                            epoch);

                for (const std::uint32_t other : candidates) {
                    if (other <= local) {
                        continue; // each pair once
                    }

                    const ImageSignature& a = work[subject[local]].signature.image;
                    const ImageSignature& b = work[subject[other]].signature.image;
                    if (!images_match(a, b, config.image)) {
                        continue;
                    }

                    // Already reported as an exact cluster.
                    if (work[subject[local]].signature.has_full_hash &&
                        work[subject[other]].signature.has_full_hash &&
                        work[subject[local]].signature.full_hash ==
                            work[subject[other]].signature.full_hash) {
                        continue;
                    }

                    pairs.push_back(MatchPair{static_cast<std::uint32_t>(local), other,
                                              hamming_distance(a.phash64, b.phash64)});
                }

                const std::uint64_t done = queried.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done % 4096 == 0) {
                    report_progress(Progress::Phase::Indexing, done, subject.size());
                }
            }
        },
        token, 1);

    std::vector<MatchPair> pairs;
    std::size_t total = 0;
    for (const auto& chunk : per_worker) {
        total += chunk.size();
    }
    pairs.reserve(total);
    for (auto& chunk : per_worker) {
        pairs.insert(pairs.end(), chunk.begin(), chunk.end());
    }

    // Grouping walks the list in order, so sorting is what makes two runs over the
    // same corpus produce identical clusters.
    std::sort(pairs.begin(), pairs.end(), [](const MatchPair& a, const MatchPair& b) {
        return a.a != b.a ? a.a < b.a : a.b < b.b;
    });

    if (pairs.empty()) {
        return {};
    }

    report_progress(Progress::Phase::Clustering, 0, pairs.size());

    const auto groups = config.cluster_mode == ClusterMode::Strict
                            ? group_strict(subject.size(), pairs)
                            : group_transitive(subject.size(), pairs);

    std::vector<Cluster> clusters;
    clusters.reserve(groups.size());

    for (const auto& group : groups) {
        std::vector<std::uint32_t> members;
        members.reserve(group.size());
        for (const std::uint32_t local : group) {
            members.push_back(subject[local]);
        }
        clusters.push_back(finalize(
            std::move(members), MatchKind::Similar, MediaKind::Image, files, keeper_info,
            [&](std::uint32_t a, std::uint32_t b) {
                return hamming_distance(work[a].signature.image.phash64,
                                        work[b].signature.image.phash64);
            }));
    }

    return clusters;
}

std::vector<Cluster> Scanner::Impl::find_similar_videos(
    const std::vector<FileEntry>& files, const std::vector<FileWork>& work,
    const std::vector<KeeperInfo>& keeper_info, const std::stop_token& token) {
    std::vector<std::uint32_t> subject;
    for (std::uint32_t i = 0; i < files.size(); ++i) {
        if (work[i].usable && work[i].signature.has_video &&
            work[i].signature.video.frame_count > 0) {
            subject.push_back(i);
        }
    }
    if (subject.size() < 2) {
        return {};
    }

    // Sorting by duration turns the O(n^2) comparison into a sliding window: the
    // scan breaks out as soon as the duration gap exceeds the tolerance.
    std::sort(subject.begin(), subject.end(), [&](std::uint32_t a, std::uint32_t b) {
        return work[a].signature.video.duration_ms < work[b].signature.video.duration_ms;
    });

    std::vector<MatchPair> pairs;

    for (std::uint32_t i = 0; i < subject.size(); ++i) {
        if (token.stop_possible() && token.stop_requested()) {
            break;
        }

        const VideoSignature& a = work[subject[i]].signature.video;

        for (std::uint32_t j = i + 1; j < subject.size(); ++j) {
            const VideoSignature& b = work[subject[j]].signature.video;

            // Sub-clip detection compares across durations, so the window is off
            // whenever it is on.
            if (!config.video.subclip_detection && a.duration_ms > 0 && b.duration_ms > 0) {
                const double longer = static_cast<double>(std::max(a.duration_ms, b.duration_ms));
                if (static_cast<double>(b.duration_ms - a.duration_ms) / longer >
                    config.video.duration_tolerance) {
                    break; // sorted, so everything beyond is further away still
                }
            }

            const double similarity = video_similarity(a, b, config.video);
            if (similarity < config.video.min_frame_match_ratio) {
                continue;
            }
            if (work[subject[i]].signature.has_full_hash &&
                work[subject[j]].signature.has_full_hash &&
                work[subject[i]].signature.full_hash == work[subject[j]].signature.full_hash) {
                continue; // already an exact cluster
            }

            pairs.push_back(MatchPair{
                i, j, static_cast<std::uint32_t>((1.0 - similarity) * 100.0)});
        }
    }

    if (pairs.empty()) {
        return {};
    }

    const auto groups = config.cluster_mode == ClusterMode::Strict
                            ? group_strict(subject.size(), pairs)
                            : group_transitive(subject.size(), pairs);

    std::vector<Cluster> clusters;
    clusters.reserve(groups.size());

    for (const auto& group : groups) {
        std::vector<std::uint32_t> members;
        members.reserve(group.size());
        for (const std::uint32_t local : group) {
            members.push_back(subject[local]);
        }
        clusters.push_back(finalize(
            std::move(members), MatchKind::Similar, MediaKind::Video, files, keeper_info,
            [&](std::uint32_t a, std::uint32_t b) {
                const double similarity = video_similarity(
                    work[a].signature.video, work[b].signature.video, config.video);
                return static_cast<std::uint32_t>((1.0 - similarity) * 100.0);
            }));
    }

    return clusters;
}

Cluster Scanner::Impl::finalize(
    std::vector<std::uint32_t> members, MatchKind kind, MediaKind media,
    const std::vector<FileEntry>& files, const std::vector<KeeperInfo>& keeper_info,
    const std::function<std::uint32_t(std::uint32_t, std::uint32_t)>& distance) {
    Cluster cluster;
    cluster.kind = kind;
    cluster.media = media;
    cluster.members = std::move(members);
    cluster.keeper = choose_keeper(cluster.members, keeper_info, config.keeper_policy);

    cluster.distances.reserve(cluster.members.size());
    for (const std::uint32_t member : cluster.members) {
        cluster.distances.push_back(member == cluster.keeper ? 0U
                                                             : distance(cluster.keeper, member));
        if (member != cluster.keeper) {
            cluster.reclaimable_bytes += files[member].size;
        }
    }

    return cluster;
}

Result<Report> Scanner::Impl::run(std::span<const std::filesystem::path> roots,
                                  std::stop_token token) {
    if (auto valid = config.validate(); !valid) {
        return valid.error();
    }
    if (roots.empty()) {
        return Error{ErrorCode::InvalidArgument, "no scan roots were given"};
    }

    const auto started = std::chrono::steady_clock::now();
    Report report;

    report_progress(Progress::Phase::Enumerating, 0, 0);
    auto walked = enumerate_files(roots, config, token);
    report.files = std::move(walked.files);
    report.errors = std::move(walked.errors);
    report.stats.files_seen = walked.files_seen;
    report.stats.hardlinks_collapsed = walked.hardlinks_collapsed;
    report.cancelled = walked.cancelled;

    if (report.files.empty()) {
        report.stats.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return report;
    }

    SignatureCache cache;
    SignatureCache* cache_ptr = nullptr;
    if (config.cache.enabled) {
        std::filesystem::path cache_path = config.cache.path;
        if (cache_path.empty()) {
            // Next to the scanned files, never in a user-wide directory: an opt-in
            // cache stays where the user can see and delete it.
            std::error_code ec;
            const std::filesystem::path& root = roots.front();
            cache_path = (std::filesystem::is_directory(root, ec) ? root : root.parent_path()) /
                         "ghidraengine-cache.db";
        }
        if (auto opened = cache.open(cache_path); opened) {
            if (auto loaded = cache.load(); loaded) {
                cache_ptr = &cache;
            }
        }
        // A cache that will not open is a performance loss, not a correctness
        // problem, so the scan proceeds either way.
    }

    const unsigned cpu_threads = config.concurrency.cpu_threads != 0
                                     ? config.concurrency.cpu_threads
                                     : ThreadPool::default_thread_count();

    unsigned io_threads = config.concurrency.io_threads;
    if (io_threads == 0) {
        // Concurrent reads are a win on flash and a seek storm on rotational media.
        io_threads = platform::is_rotational_storage(roots.front()) ? 2u : cpu_threads;
    }

    ThreadPool pool(cpu_threads);
    IoLimiter io_limiter(std::max(1u, std::min(io_threads, cpu_threads + 1u)));

    std::vector<FileWork> work(report.files.size());
    std::mutex guard;
    std::atomic<std::uint64_t> processed{0};

    report_progress(Progress::Phase::Decoding, 0, report.files.size());

    parallel_for(
        pool, 0, report.files.size(),
        [&](std::size_t index) {
            process_file(report.files[index], work[index], cache_ptr, io_limiter,
                         report.errors, report.stats, guard);

            const std::uint64_t done = processed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done % 128 == 0) {
                report_progress(Progress::Phase::Decoding, done, report.files.size());
            }
        },
        token);

    if (token.stop_possible() && token.stop_requested()) {
        report.cancelled = true;
    }

    for (const FileWork& item : work) {
        if (item.usable) {
            ++report.stats.files_considered;
        }
    }

    std::vector<KeeperInfo> keeper_info(report.files.size());
    for (std::size_t i = 0; i < report.files.size(); ++i) {
        keeper_info[i].pixels = pixel_count(work[i]);
        keeper_info[i].size = report.files[i].size;
        keeper_info[i].mtime_ns = report.files[i].mtime_ns;
        keeper_info[i].path_length = report.files[i].path.native().size();
        report.files[i].media = work[i].media;
    }

    if (config.detect_exact) {
        auto exact = find_exact_clusters(report.files, work, keeper_info, pool, token,
                                         report.errors);
        report.clusters.insert(report.clusters.end(), std::make_move_iterator(exact.begin()),
                               std::make_move_iterator(exact.end()));
    }

    if (config.detect_similar && config.scan_images) {
        auto similar = find_similar_images(report.files, work, keeper_info, pool, token);
        report.clusters.insert(report.clusters.end(), std::make_move_iterator(similar.begin()),
                               std::make_move_iterator(similar.end()));
    }

    if (config.detect_similar && config.scan_videos) {
        auto similar = find_similar_videos(report.files, work, keeper_info, token);
        report.clusters.insert(report.clusters.end(), std::make_move_iterator(similar.begin()),
                               std::make_move_iterator(similar.end()));
    }

    // Largest savings first: that is the order a user acts on.
    std::sort(report.clusters.begin(), report.clusters.end(),
              [](const Cluster& a, const Cluster& b) {
                  if (a.reclaimable_bytes != b.reclaimable_bytes) {
                      return a.reclaimable_bytes > b.reclaimable_bytes;
                  }
                  return a.members.front() < b.members.front();
              });

    if (cache_ptr != nullptr) {
        cache_ptr->flush(config.cache.prune_after_days);
    }

    report.stats.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    report_progress(Progress::Phase::Done, report.files.size(), report.files.size());

    return report;
}

Scanner::Scanner(ScanConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

Scanner::~Scanner() = default;
Scanner::Scanner(Scanner&&) noexcept = default;
Scanner& Scanner::operator=(Scanner&&) noexcept = default;

const ScanConfig& Scanner::config() const noexcept { return impl_->config; }

void Scanner::set_config(ScanConfig config) { impl_->config = std::move(config); }

Result<Report> Scanner::scan(std::span<const std::filesystem::path> roots) {
    return impl_->run(roots, impl_->internal_stop.get_token());
}

Result<Report> Scanner::scan(std::span<const std::filesystem::path> roots,
                             std::stop_token token) {
    return impl_->run(roots, std::move(token));
}

const char* version_string() noexcept { return "0.1.0"; }

MediaKind probe_media_kind(std::span<const std::uint8_t> header) noexcept {
    return classify_header(header);
}

MediaKind probe_media_kind(const std::filesystem::path& path) {
    auto file = platform::File::open_read(path, /*sequential=*/false);
    if (!file) {
        return MediaKind::Unknown;
    }
    std::array<std::uint8_t, kHeaderBytes> header{};
    auto read = read_header(*file, std::span<std::uint8_t, kHeaderBytes>(header));
    if (!read) {
        return MediaKind::Unknown;
    }
    return classify_header(std::span<const std::uint8_t>(header.data(), read.value()));
}

Result<Hash128> hash_file(const std::filesystem::path& path) {
    auto file = platform::File::open_read(path, /*sequential=*/true);
    if (!file) {
        return file.error();
    }
    std::vector<std::uint8_t> buffer(kStreamBufferSize);
    return hash_full(*file, buffer, nullptr);
}

Result<Hash128> hash_file_partial(const std::filesystem::path& path, std::uint64_t size) {
    auto file = platform::File::open_read(path, /*sequential=*/false);
    if (!file) {
        return file.error();
    }
    std::vector<std::uint8_t> buffer(kPartialHashChunk * 2);
    return hash_partial(*file, size, buffer);
}

Result<ImageSignature> compute_image_signature(const std::filesystem::path& path,
                                               const ImageMatchConfig& config) {
    std::vector<std::uint8_t> buffer;
    if (auto read = read_entire_file(path, buffer, kMaxImageBytes); !read) {
        return read.error();
    }
    auto thumb = decode_image(buffer);
    if (!thumb) {
        return thumb.error();
    }
    return compute_signature(*thumb, config);
}

Result<VideoSignature> compute_video_signature(const std::filesystem::path& path,
                                               const VideoMatchConfig& config) {
    return extract_video_signature(path, config);
}

} // namespace ghidraengine
