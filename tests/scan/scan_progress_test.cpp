#include <GhidraEngine/scan/scanner.hpp>

#include "../support/temp_directory.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <fstream>
#include <thread>

namespace GhidraEngine {

namespace {

using test_support::TempDirectory;

void write_file(const std::filesystem::path &path, const std::string &value = "duplicate") {
    std::ofstream output(path, std::ios::binary);
    output << value;
    ASSERT_TRUE(output.good());
}

TEST(Scanner, ExactIgnoresFingerprintSettingsAndCache) {
    const TempDirectory directory;
    write_file(directory.path() / "a");
    const FingerprintCache cache(directory.path() / "cache");
    write_file(cache.directory() / "file");
    ScanOptions options;
    options.generate_fingerprints = false;
    options.fingerprints.video.decode.sample_interval = Timestamp{-1};
    const auto result = scan_media(std::array{directory.path()}, options, &cache);
    EXPECT_EQ(result.files.size(), 2);
    EXPECT_TRUE(result.issues.empty());
}

TEST(Scanner, ProgressIsSerializedAndMonotonic) {
    const TempDirectory directory;

    for (int i = 0; i < 32; ++i) {
        write_file(directory.path() / std::to_string(i));
    }
    ScanOptions options;
    options.generate_fingerprints = false;
    options.workers = 4;
    std::atomic<int> active = 0;
    std::size_t completed = 0;
    std::size_t discovered = 0;
    const auto result = scan_media(
        std::array{directory.path()},
        options,
        nullptr,
        {},
        [&](const ScanProgress &progress) {
            EXPECT_EQ(active.fetch_add(1), 0);
            EXPECT_GE(progress.discovered, discovered);
            EXPECT_GE(progress.completed, completed);
            discovered = progress.discovered;
            completed = progress.completed;
            if (progress.stage == ScanProgressStage::Processing) {
                EXPECT_EQ(progress.total, 32);
            }
            std::this_thread::yield();
            EXPECT_EQ(active.fetch_sub(1), 1);
        }
    );
    EXPECT_EQ(completed, 32);
    EXPECT_EQ(result.files.size(), 32);
}

TEST(Scanner, CallbackExceptionPropagatesAfterWorkersJoin) {
    const TempDirectory directory;
    write_file(directory.path() / "a");
    write_file(directory.path() / "b");
    ScanOptions options;
    options.generate_fingerprints = false;
    options.workers = 2;
    EXPECT_THROW(
        (void)scan_media(
            std::array{directory.path()},
            options,
            nullptr,
            {},
            [](const ScanProgress &progress) {
                if (progress.completed == 1) {
                    throw std::logic_error("observer failure");
                }
            }
        ),
        std::logic_error
    );
}

} // namespace

} // namespace GhidraEngine
