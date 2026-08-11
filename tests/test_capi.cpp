// Boundary invariants: defaults filled in, null arguments rejected rather than
// crashing, returned pointers valid for the handle's lifetime, no exception out.
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ghidraengine/ghidraengine_c.h"
#include "image_fixture.hpp"

using namespace ghidraengine::test;

TEST_CASE("C API reports version and defaults", "[capi]") {
    CHECK(std::strlen(ghidraengine_version_string()) > 0);
    CHECK(std::strlen(ghidraengine_simd_backend()) > 0);
    CHECK(std::string(ghidraengine_status_message(GHIDRAENGINE_OK)) == "ok");

    ghidraengine_config config;
    std::memset(&config, 0xAB, sizeof(config)); // poison, so defaults must overwrite
    ghidraengine_config_init(&config);

    CHECK(config.detect_exact == 1);
    CHECK(config.detect_similar == 1);
    CHECK(config.phash_threshold == 10);
    CHECK(config.video_frame_samples == 16);
    CHECK(config.cache_enabled == 0);
    CHECK(config.cache_path == nullptr);
    CHECK(config.cluster_mode == GHIDRAENGINE_CLUSTER_STRICT);
}

TEST_CASE("C API rejects bad arguments without crashing", "[capi]") {
    ghidraengine_scanner* scanner = nullptr;

    CHECK(ghidraengine_scanner_create(nullptr, nullptr) == GHIDRAENGINE_ERR_INVALID_ARGUMENT);
    CHECK(ghidraengine_scanner_create(nullptr, &scanner) == GHIDRAENGINE_OK);
    REQUIRE(scanner != nullptr);

    CHECK(ghidraengine_scanner_scan(scanner, nullptr, 1, nullptr) == GHIDRAENGINE_ERR_INVALID_ARGUMENT);

    ghidraengine_report* report = nullptr;
    const char* roots[] = {nullptr};
    CHECK(ghidraengine_scanner_scan(scanner, roots, 1, &report) == GHIDRAENGINE_ERR_INVALID_ARGUMENT);

    ghidraengine_scanner_free(scanner);

    SECTION("a contradictory config is refused at creation") {
        ghidraengine_config config;
        ghidraengine_config_init(&config);
        config.detect_exact = 0;
        config.detect_similar = 0;

        ghidraengine_scanner* rejected = nullptr;
        CHECK(ghidraengine_scanner_create(&config, &rejected) == GHIDRAENGINE_ERR_INVALID_ARGUMENT);
        CHECK(rejected == nullptr);
    }

    SECTION("accessors tolerate null and out-of-range input") {
        CHECK(ghidraengine_report_cluster_count(nullptr) == 0);
        CHECK(std::string(ghidraengine_report_file_path(nullptr, 0)).empty());
        CHECK(ghidraengine_report_total_reclaimable(nullptr) == 0);
        ghidraengine_report_free(nullptr);
    }
}

TEST_CASE("C API finds duplicates end to end", "[capi]") {
    TempDir dir;

    const auto photo = encode_jpeg(make_image(500, 400, 61), 90);
    write_file(dir.path() / "a.jpg", photo);
    write_file(dir.path() / "b.jpg", photo);
    write_file(dir.path() / "c.jpg", encode_jpeg(make_image(500, 400, 62), 90));

    const std::string root = dir.path().string();
    const std::string cache = (dir.path() / "cache.db").string();

    ghidraengine_config config;
    ghidraengine_config_init(&config);
    config.min_file_size = 0;
    config.cache_enabled = 1;
    config.cache_path = cache.c_str();
    config.detect_similar = 0;

    ghidraengine_scanner* scanner = nullptr;
    REQUIRE(ghidraengine_scanner_create(&config, &scanner) == GHIDRAENGINE_OK);

    int progress_calls = 0;
    ghidraengine_scanner_set_progress(
        scanner,
        [](int, uint64_t, uint64_t, void* user_data) {
            ++*static_cast<int*>(user_data);
            return 0; // non-zero would request cancellation
        },
        &progress_calls);

    ghidraengine_report* report = nullptr;
    const char* roots[] = {root.c_str()};
    REQUIRE(ghidraengine_scanner_scan(scanner, roots, 1, &report) == GHIDRAENGINE_OK);
    REQUIRE(report != nullptr);

    CHECK(ghidraengine_report_file_count(report) == 3);
    REQUIRE(ghidraengine_report_cluster_count(report) == 1);
    CHECK(ghidraengine_report_cluster_match_kind(report, 0) == GHIDRAENGINE_MATCH_EXACT);
    CHECK(ghidraengine_report_cluster_media_kind(report, 0) == GHIDRAENGINE_MEDIA_IMAGE);
    CHECK(ghidraengine_report_cluster_member_count(report, 0) == 2);
    CHECK(ghidraengine_report_cluster_reclaimable(report, 0) == photo.size());
    CHECK(ghidraengine_report_total_reclaimable(report) == photo.size());
    CHECK(progress_calls > 0);

    // Every member index must address a real file readable through the accessor.
    for (size_t i = 0; i < ghidraengine_report_cluster_member_count(report, 0); ++i) {
        const uint32_t member = ghidraengine_report_cluster_member(report, 0, i);
        REQUIRE(member < ghidraengine_report_file_count(report));
        const char* path = ghidraengine_report_file_path(report, member);
        CHECK(std::strlen(path) > 0);
        CHECK(ghidraengine_report_file_size(report, member) == photo.size());
        CHECK(ghidraengine_report_file_media_kind(report, member) == GHIDRAENGINE_MEDIA_IMAGE);
    }

    uint64_t files_seen = 0;
    uint64_t considered = 0;
    double elapsed = 0.0;
    ghidraengine_report_stats(report, &files_seen, &considered, nullptr, nullptr, nullptr, nullptr,
                         nullptr, &elapsed);
    CHECK(considered == 3);
    CHECK(elapsed >= 0.0);

    ghidraengine_report_free(report);
    ghidraengine_scanner_free(scanner);
}

TEST_CASE("C API cancellation through the progress callback", "[capi]") {
    TempDir dir;
    for (int i = 0; i < 12; ++i) {
        write_file(dir.path() / (std::to_string(i) + ".jpg"),
                   encode_jpeg(make_image(400, 300, static_cast<std::uint32_t>(i)), 85));
    }

    const std::string root = dir.path().string();

    ghidraengine_config config;
    ghidraengine_config_init(&config);
    config.min_file_size = 0;
    config.cache_enabled = 0;

    ghidraengine_scanner* scanner = nullptr;
    REQUIRE(ghidraengine_scanner_create(&config, &scanner) == GHIDRAENGINE_OK);

    ghidraengine_scanner_set_progress(
        scanner, [](int, uint64_t, uint64_t, void*) { return 1; }, nullptr);

    ghidraengine_report* report = nullptr;
    const char* roots[] = {root.c_str()};
    REQUIRE(ghidraengine_scanner_scan(scanner, roots, 1, &report) == GHIDRAENGINE_OK);
    REQUIRE(report != nullptr);

    CHECK(ghidraengine_report_was_cancelled(report) == 1);

    ghidraengine_report_free(report);
    ghidraengine_scanner_free(scanner);
}
