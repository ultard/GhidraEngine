#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "ghidraengine/ghidraengine.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using namespace ghidraengine;

std::string human_bytes(std::uint64_t bytes) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.2f %s", value,
                  units[unit]);
    return buffer;
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

// CLI11's ensure_utf8 guarantees argv is UTF-8 on every platform.
std::filesystem::path to_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

void print_text_report(std::ostream& out, const Report& report, bool show_all) {
    out << "\n";
    if (report.clusters.empty()) {
        out << "No duplicates found.\n";
    }

    std::size_t index = 0;
    for (const Cluster& cluster : report.clusters) {
        ++index;
        out << "[" << index << "] " << to_string(cluster.kind) << " "
            << to_string(cluster.media) << ", " << cluster.members.size() << " files, "
            << human_bytes(cluster.reclaimable_bytes) << " reclaimable\n";

        for (std::size_t i = 0; i < cluster.members.size(); ++i) {
            const std::uint32_t member = cluster.members[i];
            const bool keeper = member == cluster.keeper;
            out << "    " << (keeper ? "KEEP " : "     ")
                << to_utf8(report.files[member].path) << "  ("
                << human_bytes(report.files[member].size);
            if (!keeper && cluster.kind == MatchKind::Similar) {
                out << ", distance " << cluster.distances[i];
            }
            out << ")\n";
        }
        out << "\n";
    }

    const ScanStats& stats = report.stats;
    out << "Scanned " << stats.files_seen << " entries, " << stats.files_considered
        << " media files in " << stats.elapsed_seconds << " s\n";
    out << "  cache hits    : " << stats.cache_hits << "\n";
    out << "  images decoded: " << stats.images_decoded << "\n";
    out << "  videos probed : " << stats.videos_probed << "\n";
    out << "  bytes read    : " << human_bytes(stats.bytes_read) << "\n";
    if (stats.hardlinks_collapsed > 0) {
        out << "  hard links    : " << stats.hardlinks_collapsed << " collapsed\n";
    }
    out << "  reclaimable   : " << human_bytes(report.total_reclaimable_bytes()) << "\n";

    if (!report.errors.empty()) {
        out << "\n" << report.errors.size() << " file(s) could not be read:\n";
        const std::size_t limit = show_all ? report.errors.size()
                                           : std::min<std::size_t>(report.errors.size(), 10);
        for (std::size_t i = 0; i < limit; ++i) {
            out << "    " << to_utf8(report.errors[i].path) << ": "
                << report.errors[i].error.message << "\n";
        }
        if (limit < report.errors.size()) {
            out << "    ... and " << (report.errors.size() - limit)
                << " more (use --all-errors)\n";
        }
    }
    if (report.cancelled) {
        out << "\nScan was cancelled; results are partial.\n";
    }
}

nlohmann::json build_json(const Report& report) {
    nlohmann::json root;
    root["version"] = version_string();
    root["cancelled"] = report.cancelled;

    nlohmann::json stats;
    stats["files_seen"] = report.stats.files_seen;
    stats["files_considered"] = report.stats.files_considered;
    stats["files_hashed"] = report.stats.files_hashed;
    stats["images_decoded"] = report.stats.images_decoded;
    stats["videos_probed"] = report.stats.videos_probed;
    stats["cache_hits"] = report.stats.cache_hits;
    stats["bytes_read"] = report.stats.bytes_read;
    stats["hardlinks_collapsed"] = report.stats.hardlinks_collapsed;
    stats["elapsed_seconds"] = report.stats.elapsed_seconds;
    stats["total_reclaimable_bytes"] = report.total_reclaimable_bytes();
    root["stats"] = std::move(stats);

    nlohmann::json clusters = nlohmann::json::array();
    for (const Cluster& cluster : report.clusters) {
        nlohmann::json item;
        item["kind"] = to_string(cluster.kind);
        item["media"] = to_string(cluster.media);
        item["reclaimable_bytes"] = cluster.reclaimable_bytes;
        item["keeper"] = to_utf8(report.files[cluster.keeper].path);

        nlohmann::json members = nlohmann::json::array();
        for (std::size_t i = 0; i < cluster.members.size(); ++i) {
            const std::uint32_t index = cluster.members[i];
            nlohmann::json member;
            member["path"] = to_utf8(report.files[index].path);
            member["size"] = report.files[index].size;
            member["mtime_ns"] = report.files[index].mtime_ns;
            member["distance"] = cluster.distances[i];
            member["is_keeper"] = index == cluster.keeper;
            members.push_back(std::move(member));
        }
        item["members"] = std::move(members);
        clusters.push_back(std::move(item));
    }
    root["clusters"] = std::move(clusters);

    nlohmann::json errors = nlohmann::json::array();
    for (const FileError& error : report.errors) {
        nlohmann::json item;
        item["path"] = to_utf8(error.path);
        item["message"] = error.error.message;
        errors.push_back(std::move(item));
    }
    root["errors"] = std::move(errors);

    return root;
}

void print_csv(std::ostream& out, const Report& report) {
    out << "cluster,kind,media,is_keeper,distance,size,path\n";
    std::size_t index = 0;
    for (const Cluster& cluster : report.clusters) {
        ++index;
        for (std::size_t i = 0; i < cluster.members.size(); ++i) {
            const std::uint32_t member = cluster.members[i];
            std::string escaped = "\"";
            for (const char c : to_utf8(report.files[member].path)) {
                if (c == '"') {
                    escaped += "\"\"";
                } else {
                    escaped += c;
                }
            }
            escaped += '"';

            out << index << ',' << to_string(cluster.kind) << ','
                << to_string(cluster.media) << ',' << (member == cluster.keeper ? 1 : 0)
                << ',' << cluster.distances[i] << ',' << report.files[member].size << ','
                << escaped << "\n";
        }
    }
}

std::uint64_t apply_deletions(const Report& report, bool dry_run) {
    std::uint64_t freed = 0;
    for (const Cluster& cluster : report.clusters) {
        for (const std::uint32_t member : cluster.members) {
            if (member == cluster.keeper) {
                continue;
            }
            const std::filesystem::path& path = report.files[member].path;
            if (dry_run) {
                std::cout << "would delete " << to_utf8(path) << "\n";
                freed += report.files[member].size;
                continue;
            }
            std::error_code ec;
            if (std::filesystem::remove(path, ec)) {
                freed += report.files[member].size;
            } else {
                std::cerr << "failed to delete " << to_utf8(path) << ": " << ec.message()
                          << "\n";
            }
        }
    }
    return freed;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    CLI::App app{"ghidraengine - find duplicate photos and videos"};
    app.set_version_flag("--version", version_string());

    std::vector<std::string> roots;
    app.add_option("paths", roots, "Directories or files to scan")->required();

    ScanConfig config;
    std::string format = "text";
    std::string output_file;
    std::string cache_path;
    bool exact_only = false;
    bool similar_only = false;
    bool images_only = false;
    bool videos_only = false;
    bool use_cache = false;
    bool transitive = false;
    bool hidden = false;
    bool quiet = false;
    bool all_errors = false;
    bool delete_duplicates = false;
    bool confirm = false;
    std::string keeper = "resolution";

    app.add_option("-f,--format", format, "Output format: text, json, csv")
        ->check(CLI::IsMember({"text", "json", "csv"}));
    app.add_option("-o,--output", output_file,
                   "Write the report to a file instead of stdout");

    app.add_flag("--exact-only", exact_only, "Only byte-identical duplicates");
    app.add_flag("--similar-only", similar_only, "Only perceptually similar duplicates");
    app.add_flag("--images-only", images_only, "Skip video files");
    app.add_flag("--videos-only", videos_only, "Skip image files");

    app.add_option("-t,--threshold", config.image.phash_threshold,
                   "Image similarity threshold in Hamming bits (0-64, default 10)")
        ->check(CLI::Range(0u, 64u));
    app.add_option("--color-threshold", config.image.color_threshold,
                   "Maximum chroma difference, 0-255 (255 disables the check)")
        ->check(CLI::Range(0u, 255u));
    app.add_flag("--rotations", config.image.dihedral_invariant,
                 "Also match rotated and mirrored copies");

    app.add_option("--video-samples", config.video.frame_samples,
                   "Keyframes sampled per video (1-32, default 16)")
        ->check(CLI::Range(1u, static_cast<unsigned>(kMaxVideoFrames)));
    app.add_option("--video-match", config.video.min_frame_match_ratio,
                   "Fraction of frames that must match (0-1, default 0.65)")
        ->check(CLI::Range(0.0, 1.0));
    app.add_flag("--subclips", config.video.subclip_detection,
                 "Detect a video contained within a longer one");

    app.add_option("--min-size", config.min_file_size, "Ignore files below this many bytes");
    app.add_option("--max-size", config.max_file_size, "Ignore files above this many bytes");
    app.add_option("-x,--exclude", config.exclude_patterns,
                   "Glob pattern to exclude (repeatable)");
    app.add_option("--max-depth", config.max_depth, "Limit recursion depth (0 = unlimited)");
    app.add_flag("--hidden", hidden, "Include hidden files");
    app.add_flag("--follow-symlinks", config.follow_symlinks, "Follow symbolic links");
    app.add_flag("--probe-all", config.probe_unknown_extensions,
                 "Inspect every file, not just known media extensions");

    app.add_option("-j,--threads", config.concurrency.cpu_threads,
                   "Worker threads (0 = auto)");
    app.add_flag("--cache", use_cache,
                 "Keep a signature cache so a rescan is free; written to the first scanned "
                 "folder unless --cache-path says otherwise");
    app.add_option("--cache-path", cache_path, "Cache database location (implies --cache)");
    app.add_flag("--verify", config.verify_bytes,
                 "Confirm exact duplicates with a full byte comparison");
    app.add_flag("--transitive", transitive,
                 "Merge clusters transitively (higher recall, risks chaining)");
    app.add_option("--keep", keeper, "Keeper policy: resolution, largest, oldest, newest, shortest")
        ->check(CLI::IsMember({"resolution", "largest", "oldest", "newest", "shortest"}));

    app.add_flag("-q,--quiet", quiet, "Suppress progress output");
    app.add_flag("--all-errors", all_errors, "List every unreadable file");

    app.add_flag("--delete", delete_duplicates,
                 "Delete non-keeper members (dry run unless --confirm is also given)");
    app.add_flag("--confirm", confirm, "Actually perform deletions requested by --delete");

    // Without this, non-ASCII arguments arrive in the local ANSI code page on
    // Windows and every path built from them is garbage.
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);

    if (exact_only && similar_only) {
        std::cerr << "--exact-only and --similar-only are mutually exclusive\n";
        return 2;
    }
    if (images_only && videos_only) {
        std::cerr << "--images-only and --videos-only are mutually exclusive\n";
        return 2;
    }

    if (exact_only) {
        config.detect_similar = false;
    }
    if (similar_only) {
        config.detect_exact = false;
    }
    if (images_only) {
        config.scan_videos = false;
    }
    if (videos_only) {
        config.scan_images = false;
    }
    if (!cache_path.empty()) {
        config.cache.path = to_path(cache_path);
    }
    config.cache.enabled = use_cache || !cache_path.empty();
    if (transitive) {
        config.cluster_mode = ClusterMode::Transitive;
    }
    if (hidden) {
        config.skip_hidden = false;
    }

    if (keeper == "largest") {
        config.keeper_policy = KeeperPolicy::LargestFile;
    } else if (keeper == "oldest") {
        config.keeper_policy = KeeperPolicy::OldestModified;
    } else if (keeper == "newest") {
        config.keeper_policy = KeeperPolicy::NewestModified;
    } else if (keeper == "shortest") {
        config.keeper_policy = KeeperPolicy::ShortestPath;
    }

    if (!quiet && format == "text") {
        // Fires from worker threads, so the rate limiter has to be atomic and a
        // write per file would make stderr the bottleneck.
        auto last = std::make_shared<std::atomic<std::int64_t>>(0);
        constexpr auto kInterval =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(200))
                .count();

        config.on_progress = [last](const Progress& progress) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            auto previous = last->load(std::memory_order_relaxed);
            if (now - previous < kInterval ||
                !last->compare_exchange_strong(previous, now, std::memory_order_relaxed)) {
                return;
            }
            std::fprintf(stderr, "\r%-12s %llu", to_string(progress.phase),
                         static_cast<unsigned long long>(progress.processed));
            if (progress.total > 0) {
                std::fprintf(stderr, " / %llu",
                             static_cast<unsigned long long>(progress.total));
            }
            std::fflush(stderr);
        };
    }

    std::vector<std::filesystem::path> paths;
    paths.reserve(roots.size());
    for (const std::string& root : roots) {
        paths.push_back(to_path(root));
    }

    Scanner scanner(std::move(config));
    auto result = scanner.scan(paths);

    if (!quiet && format == "text") {
        std::fprintf(stderr, "\r%-40s\r", "");
    }

    if (!result) {
        std::cerr << "scan failed: " << result.error().message << "\n";
        return 1;
    }
    const Report& report = result.value();

    std::ofstream file;
    if (!output_file.empty()) {
        file.open(to_path(output_file), std::ios::binary);
        if (!file) {
            std::cerr << "cannot open " << output_file << " for writing\n";
            return 1;
        }
    }
    std::ostream& out = output_file.empty() ? std::cout : file;

    if (format == "json") {
        out << build_json(report).dump(2) << "\n";
    } else if (format == "csv") {
        print_csv(out, report);
    } else {
        print_text_report(out, report, all_errors);
    }

    if (delete_duplicates) {
        if (!confirm) {
            std::cout << "\n--delete given without --confirm; showing what would happen:\n";
        }
        const std::uint64_t freed = apply_deletions(report, !confirm);
        std::cout << (confirm ? "\nFreed " : "\nWould free ") << human_bytes(freed) << "\n";
    }

    return 0;
}
