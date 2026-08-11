#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ghidraengine/ghidraengine.hpp"
#include "cache/sqlite_cache.hpp"
#include "core/platform.hpp"
#include "hash/content_hash.hpp"
#include "image_fixture.hpp"

using namespace ghidraengine;
using namespace ghidraengine::test;

namespace {

ScanConfig test_config(const std::filesystem::path& cache_dir) {
    ScanConfig config;
    config.min_file_size = 0;
    config.cache.enabled = true;
    config.cache.path = cache_dir / "cache.db";
    config.concurrency.cpu_threads = 4;
    return config;
}

bool cluster_contains(const Report& report, const Cluster& cluster, const std::string& name) {
    return std::any_of(cluster.members.begin(), cluster.members.end(),
                       [&](std::uint32_t index) {
                           return report.files[index].path.filename().string() == name;
                       });
}

const Cluster* find_cluster_with(const Report& report, const std::string& name) {
    for (const Cluster& cluster : report.clusters) {
        if (cluster_contains(report, cluster, name)) {
            return &cluster;
        }
    }
    return nullptr;
}

}

TEST_CASE("exact duplicates are found across directories", "[scan]") {
    TempDir dir;

    const auto photo = encode_jpeg(make_image(600, 400, 11), 90);
    const auto other = encode_jpeg(make_image(600, 400, 22), 90);

    write_file(dir.path() / "a" / "original.jpg", photo);
    write_file(dir.path() / "b" / "copy.jpg", photo);
    write_file(dir.path() / "b" / "nested" / "copy2.jpg", photo);
    write_file(dir.path() / "a" / "different.jpg", other);

    ScanConfig config = test_config(dir.path());
    config.detect_similar = false;

    Scanner scanner(config);
    const std::vector<std::filesystem::path> roots = {dir.path()};
    auto report = scanner.scan(roots);
    REQUIRE(report.has_value());

    const Cluster* cluster = find_cluster_with(*report, "original.jpg");
    REQUIRE(cluster != nullptr);
    CHECK(cluster->kind == MatchKind::Exact);
    CHECK(cluster->members.size() == 3);
    CHECK(cluster_contains(*report, *cluster, "copy.jpg"));
    CHECK(cluster_contains(*report, *cluster, "copy2.jpg"));
    CHECK_FALSE(cluster_contains(*report, *cluster, "different.jpg"));

    CHECK(cluster->reclaimable_bytes == photo.size() * 2);
}

TEST_CASE("a file with a unique size never reaches the hasher", "[scan]") {
    TempDir dir;

    const Image image = make_image(500, 500, 5);
    write_file(dir.path() / "one.jpg", encode_jpeg(image, 90));
    write_file(dir.path() / "two.jpg", encode_jpeg(image, 60));

    ScanConfig config = test_config(dir.path());
    config.detect_similar = false;
    config.cache.enabled = false;

    Scanner scanner(config);
    const std::vector<std::filesystem::path> roots = {dir.path()};
    auto report = scanner.scan(roots);
    REQUIRE(report.has_value());

    CHECK(report->clusters.empty());
}

TEST_CASE("perceptually similar images are grouped", "[scan]") {
    TempDir dir;

    const Image original = make_image(1000, 750, 31);

    write_file(dir.path() / "original.jpg", encode_jpeg(original, 95));
    write_file(dir.path() / "recompressed.jpg", encode_jpeg(original, 55));
    write_file(dir.path() / "resized.jpg", encode_jpeg(resize(original, 500, 375), 90));
    write_file(dir.path() / "unrelated.jpg", encode_jpeg(make_image(1000, 750, 999), 95));

    ScanConfig config = test_config(dir.path());
    config.cluster_mode = ClusterMode::Transitive;

    Scanner scanner(config);
    const std::vector<std::filesystem::path> roots = {dir.path()};
    auto report = scanner.scan(roots);
    REQUIRE(report.has_value());

    const Cluster* cluster = find_cluster_with(*report, "original.jpg");
    REQUIRE(cluster != nullptr);
    CHECK(cluster->kind == MatchKind::Similar);
    CHECK(cluster->media == MediaKind::Image);
    CHECK(cluster_contains(*report, *cluster, "recompressed.jpg"));
    CHECK(cluster_contains(*report, *cluster, "resized.jpg"));
    CHECK_FALSE(cluster_contains(*report, *cluster, "unrelated.jpg"));

    CHECK(report->files[cluster->keeper].path.filename() == "original.jpg");
}

TEST_CASE("exclusion patterns and size filters are honoured", "[scan]") {
    TempDir dir;

    const auto photo = encode_jpeg(make_image(400, 300, 8), 90);
    write_file(dir.path() / "keep" / "a.jpg", photo);
    write_file(dir.path() / "keep" / "b.jpg", photo);
    write_file(dir.path() / "thumbs" / "a.jpg", photo);
    write_file(dir.path() / "thumbs" / "b.jpg", photo);

    ScanConfig config = test_config(dir.path());
    config.detect_similar = false;
    config.exclude_patterns.push_back("**/thumbs/**");

    Scanner scanner(config);
    const std::vector<std::filesystem::path> roots = {dir.path()};
    auto report = scanner.scan(roots);
    REQUIRE(report.has_value());

    for (const FileEntry& file : report->files) {
        CHECK(file.path.string().find("thumbs") == std::string::npos);
    }
    REQUIRE(report->clusters.size() == 1);
    CHECK(report->clusters[0].members.size() == 2);
}

TEST_CASE("a non-media file is ignored even with a media extension", "[scan]") {
    TempDir dir;

    const std::string text = "this is definitely not a JPEG, whatever the name says";
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    write_file(dir.path() / "fake1.jpg", bytes);
    write_file(dir.path() / "fake2.jpg", bytes);

    ScanConfig config = test_config(dir.path());
    Scanner scanner(config);
    const std::vector<std::filesystem::path> roots = {dir.path()};
    auto report = scanner.scan(roots);
    REQUIRE(report.has_value());

    CHECK(report->clusters.empty());
    CHECK(report->stats.files_considered == 0);
}

TEST_CASE("the cache makes a second scan free", "[scan][cache]") {
    TempDir dir;

    for (std::uint32_t i = 0; i < 8; ++i) {
        write_file(dir.path() / "photos" / (std::to_string(i) + ".jpg"),
                   encode_jpeg(make_image(500, 400, i), 90));
    }

    ScanConfig config = test_config(dir.path());
    const std::vector<std::filesystem::path> roots = {dir.path() / "photos"};

    std::uint64_t first_decodes = 0;
    {
        Scanner scanner(config);
        auto report = scanner.scan(roots);
        REQUIRE(report.has_value());
        first_decodes = report->stats.images_decoded;
        CHECK(first_decodes == 8);
        CHECK(report->stats.cache_hits == 0);
    }

    {
        Scanner scanner(config);
        auto report = scanner.scan(roots);
        REQUIRE(report.has_value());
        CHECK(report->stats.images_decoded == 0);
        CHECK(report->stats.cache_hits == 8);
    }

    SECTION("modifying a file invalidates only its own row") {
        write_file(dir.path() / "photos" / "3.jpg",
                   encode_jpeg(make_image(500, 400, 12345), 90));

        Scanner scanner(config);
        auto report = scanner.scan(roots);
        REQUIRE(report.has_value());
        CHECK(report->stats.images_decoded == 1);
        CHECK(report->stats.cache_hits == 7);
    }
}

TEST_CASE("a cached row is not reused by a scan asking a different question",
          "[scan][cache]") {
    TempDir dir;

    const Image original = make_image(800, 600, 77);
    write_file(dir.path() / "original.jpg", encode_jpeg(original, 95));
    write_file(dir.path() / "recompressed.jpg", encode_jpeg(original, 55));

    const std::vector<std::filesystem::path> roots = {dir.path()};

    SECTION("an exact-only scan does not poison the similar pass that follows") {
        ScanConfig exact_only = test_config(dir.path());
        exact_only.detect_similar = false;
        {
            Scanner scanner(exact_only);
            auto report = scanner.scan(roots);
            REQUIRE(report.has_value());
            CHECK(report->stats.images_decoded == 0);
        }

        ScanConfig full = test_config(dir.path());
        Scanner scanner(full);
        auto report = scanner.scan(roots);
        REQUIRE(report.has_value());

        CHECK(report->stats.cache_hits == 0);
        CHECK(report->stats.images_decoded == 2);
        REQUIRE(report->clusters.size() == 1);
        CHECK(report->clusters[0].kind == MatchKind::Similar);
    }

    SECTION("toggling dihedral invariance invalidates the stored hashes") {
        ScanConfig plain = test_config(dir.path());
        {
            Scanner scanner(plain);
            auto report = scanner.scan(roots);
            REQUIRE(report.has_value());
            CHECK(report->stats.cache_hits == 0);
        }

        ScanConfig rotated = test_config(dir.path());
        rotated.image.dihedral_invariant = true;

        Scanner scanner(rotated);
        auto report = scanner.scan(roots);
        REQUIRE(report.has_value());
        CHECK(report->stats.cache_hits == 0);
        CHECK(report->stats.images_decoded == 2);
    }

    SECTION("an unchanged configuration still hits") {
        ScanConfig config = test_config(dir.path());
        {
            Scanner scanner(config);
            REQUIRE(scanner.scan(roots).has_value());
        }
        Scanner scanner(config);
        auto report = scanner.scan(roots);
        REQUIRE(report.has_value());
        CHECK(report->stats.cache_hits == 2);
    }
}

TEST_CASE("an exact-only scan never reads the middle of a file", "[scan]") {
    TempDir dir;

    const Image big = make_image(3000, 2000, 41);
    const auto photo = encode_jpeg(big, 98);
    REQUIRE(photo.size() > 2 * kPartialHashChunk);

    write_file(dir.path() / "a.jpg", photo);
    write_file(dir.path() / "b.jpg", photo);

    ScanConfig config = test_config(dir.path());
    config.detect_similar = false;
    config.cache.enabled = false;

    Scanner scanner(config);
    const std::vector<std::filesystem::path> roots = {dir.path()};
    auto report = scanner.scan(roots);
    REQUIRE(report.has_value());

    REQUIRE(report->clusters.size() == 1);
    CHECK(report->clusters[0].members.size() == 2);

    CHECK(report->stats.bytes_read <= 4 * kPartialHashChunk);
}

TEST_CASE("reclaimable bytes count each file once", "[scan]") {
    TempDir dir;

    const Image original = make_image(900, 700, 63);
    const auto identical = encode_jpeg(original, 95);

    write_file(dir.path() / "a.jpg", identical);
    write_file(dir.path() / "b.jpg", identical);
    write_file(dir.path() / "c.jpg", encode_jpeg(original, 50));

    ScanConfig config = test_config(dir.path());
    config.cache.enabled = false;

    Scanner scanner(config);
    const std::vector<std::filesystem::path> roots = {dir.path()};
    auto report = scanner.scan(roots);
    REQUIRE(report.has_value());
    REQUIRE(report->clusters.size() >= 1);

    std::uint64_t total_size = 0;
    for (const FileEntry& file : report->files) {
        total_size += file.size;
    }

    CHECK(report->total_reclaimable_bytes() < total_size);

    std::uint64_t summed = 0;
    for (const Cluster& cluster : report->clusters) {
        summed += cluster.reclaimable_bytes;
    }
    CHECK(report->total_reclaimable_bytes() <= summed);
}

TEST_CASE("the cache does not trust what it reads back", "[cache]") {
    TempDir dir;

    SECTION("an out-of-range frame count is clamped on load") {
        CacheEntry entry;
        entry.key.path = "video.mp4";
        entry.key.size = 1234;
        entry.key.mtime_ns = 5678;
        entry.media = MediaKind::Video;
        entry.signature.has_video = true;
        entry.signature.video.frame_count = 9999;

        {
            SignatureCache cache;
            REQUIRE(cache.open(dir.path() / "cache.db").has_value());
            cache.store(entry);
            REQUIRE(cache.flush(0).has_value());
        }

        SignatureCache reopened;
        REQUIRE(reopened.open(dir.path() / "cache.db").has_value());
        REQUIRE(reopened.load().has_value());

        MediaKind media = MediaKind::Unknown;
        Signature signature;
        REQUIRE(reopened.lookup(entry.key, media, signature));
        CHECK(signature.video.frame_count <= kMaxVideoFrames);
    }
}

TEST_CASE("the cache commits in batches rather than at the end", "[cache]") {
    TempDir dir;

    SignatureCache cache;
    REQUIRE(cache.open(dir.path() / "cache.db").has_value());

    for (std::size_t i = 0; i < 5000; ++i) {
        CacheEntry entry;
        entry.key.path = "file" + std::to_string(i) + ".jpg";
        entry.key.size = i;
        entry.media = MediaKind::Image;
        entry.signature.has_partial_hash = true;
        cache.store(std::move(entry));
    }

    CHECK(cache.pending_rows() < 5000);

    REQUIRE(cache.flush(0).has_value());
    CHECK(cache.pending_rows() == 0);

    SignatureCache reopened;
    REQUIRE(reopened.open(dir.path() / "cache.db").has_value());
    REQUIRE(reopened.load().has_value());
    CHECK(reopened.loaded_rows() == 5000);
}

TEST_CASE("content hashing primitives", "[hash]") {
    TempDir dir;

    std::vector<std::uint8_t> payload(300 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i * 31 + 7);
    }

    const auto a = dir.path() / "a.bin";
    const auto b = dir.path() / "b.bin";
    write_file(a, payload);
    write_file(b, payload);

    auto hash_a = hash_file(a);
    auto hash_b = hash_file(b);
    REQUIRE(hash_a.has_value());
    REQUIRE(hash_b.has_value());
    CHECK(hash_a.value() == hash_b.value());

    SECTION("a single changed byte in the middle changes the full hash") {
        payload[150 * 1024] ^= 0xFF;
        write_file(b, payload);

        auto changed = hash_file(b);
        REQUIRE(changed.has_value());
        CHECK_FALSE(changed.value() == hash_a.value());
    }

    SECTION("byte comparison confirms identity") {
        auto identical = files_identical(a, b, payload.size());
        REQUIRE(identical.has_value());
        CHECK(identical.value());
    }

    SECTION("size is folded into the partial hash") {
        std::vector<std::uint8_t> shorter(payload.begin(), payload.end() - 1024);
        const auto c = dir.path() / "c.bin";
        write_file(c, shorter);

        auto partial_a = hash_file_partial(a, payload.size());
        auto partial_c = hash_file_partial(c, shorter.size());
        REQUIRE(partial_a.has_value());
        REQUIRE(partial_c.has_value());
        CHECK_FALSE(partial_a.value() == partial_c.value());
    }
}

TEST_CASE("scanning handles awkward inputs", "[scan]") {
    SECTION("a missing root is reported, not fatal") {
        TempDir dir;
        ScanConfig config = test_config(dir.path());
        Scanner scanner(config);

        const std::vector<std::filesystem::path> roots = {dir.path() / "does_not_exist"};
        auto report = scanner.scan(roots);
        REQUIRE(report.has_value());
        CHECK(report->errors.size() == 1);
        CHECK(report->errors[0].error.code == ErrorCode::NotFound);
    }

    SECTION("no roots at all is an argument error") {
        ScanConfig config;
        Scanner scanner(config);
        auto report = scanner.scan(std::vector<std::filesystem::path>{});
        REQUIRE_FALSE(report.has_value());
        CHECK(report.error().code == ErrorCode::InvalidArgument);
    }

    SECTION("an empty directory yields an empty report") {
        TempDir dir;
        ScanConfig config = test_config(dir.path());
        Scanner scanner(config);
        const std::vector<std::filesystem::path> roots = {dir.path()};
        auto report = scanner.scan(roots);
        REQUIRE(report.has_value());
        CHECK(report->clusters.empty());
        CHECK(report->files.empty());
    }

    SECTION("a contradictory configuration is rejected") {
        ScanConfig config;
        config.detect_exact = false;
        config.detect_similar = false;
        CHECK_FALSE(config.validate().has_value());
    }
}

TEST_CASE("cancellation stops the scan", "[scan]") {
    TempDir dir;
    for (std::uint32_t i = 0; i < 20; ++i) {
        write_file(dir.path() / (std::to_string(i) + ".jpg"),
                   encode_jpeg(make_image(400, 300, i), 85));
    }

    ScanConfig config = test_config(dir.path());
    config.cache.enabled = false;

    std::stop_source stop;
    stop.request_stop();

    Scanner scanner(config);
    const std::vector<std::filesystem::path> roots = {dir.path()};
    auto report = scanner.scan(roots, stop.get_token());
    REQUIRE(report.has_value());
    CHECK(report->cancelled);
}
