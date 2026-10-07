#include <GhidraEngine/hash/blake3.hpp>
#include <GhidraEngine/scan/scanner.hpp>

#include "../support/test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cstddef>
#include <future>
#include <limits>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <cstring>
#endif

namespace {

using namespace GhidraEngine;
using namespace GhidraEngine::test_support;

const MediaFingerprint image_value = ImageSignature{{{PdqHash{{1, 2, 3, 4}}, PdqQuality{50}}}};
const MediaFingerprint video_value = VpdqSignature{
    {{prefix_hash(256), PdqQuality{100}, Timestamp{-1}}, {{}, PdqQuality{0}, Timestamp{2}}}
};

TEST(Scanner, HdrImagesAndVideosSurviveParallelScanAndCacheRoundtrip) {
    TempDirectory directory;
    const auto media = directory.path() / "media";
    std::filesystem::create_directory(media);

    for (const auto *name :
         {"image/hdr-float.tiff", "image/hdr-radiance.hdr", "video/hdr10.mkv", "video/hlg.mkv"}) {
        const auto source = fixture(name);
        std::filesystem::copy_file(source, media / source.filename());
    }

    const FingerprintCache cache(directory.path() / "cache");
    const std::array roots{media};
    ScanOptions options;
    options.workers = 4;
    const auto first = scan_media(roots, options, &cache);
    const auto warm = scan_media(roots, options, &cache);

    ASSERT_EQ(first.files.size(), 4U);
    ASSERT_EQ(warm.files.size(), first.files.size());
    EXPECT_TRUE(first.issues.empty());
    EXPECT_TRUE(warm.issues.empty());

    for (std::size_t i = 0; i < first.files.size(); ++i) {
        ASSERT_TRUE(first.files[i].fingerprint);
        EXPECT_TRUE(warm.files[i].cache_hit);
        EXPECT_EQ(warm.files[i].fingerprint, first.files[i].fingerprint);
    }
}

#ifdef _WIN32
std::error_code create_junction(
    const std::filesystem::path &path,
    const std::filesystem::path &target
) {
    struct Header {
        DWORD tag;
        WORD length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
    };

    static_assert(sizeof(Header) == 16);
    const auto name = L"\\??\\" + target.native();
    const auto names_size = (name.size() + 2) * sizeof(wchar_t);

    if (sizeof(Header) + names_size > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        return std::make_error_code(std::errc::filename_too_long);
    }

    const Header header{
        IO_REPARSE_TAG_MOUNT_POINT,
        static_cast<WORD>(8 + names_size),
        0,
        0,
        static_cast<WORD>(name.size() * sizeof(wchar_t)),
        static_cast<WORD>((name.size() + 1) * sizeof(wchar_t)),
        0
    };

    std::vector<std::byte> bytes(sizeof(Header) + names_size);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), name.data(), name.size() * sizeof(wchar_t));
    std::filesystem::create_directory(path);

    const auto handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );

    if (handle == INVALID_HANDLE_VALUE) {
        return {static_cast<int>(GetLastError()), std::system_category()};
    }

    DWORD returned = 0;
    const bool created = DeviceIoControl(
        handle,
        FSCTL_SET_REPARSE_POINT,
        bytes.data(),
        static_cast<DWORD>(bytes.size()),
        nullptr,
        0,
        &returned,
        nullptr
    ) != 0;

    const auto error = created ? std::error_code{} :
        std::error_code(static_cast<int>(GetLastError()), std::system_category());
    CloseHandle(handle);

    return error;
}

TEST(Scanner, DirectoryJunctionsAreRejectedAsRootsAndNeverTraversed) {
    TempDirectory directory;
    const auto input = directory.path() / "input";

    std::filesystem::create_directory(input);
    std::filesystem::copy_file(fixture("image/bridge.png"), input / "image.png");

    const auto junction = input / "cycle";
    const auto error = create_junction(junction, input);

    if (error) {
        GTEST_SKIP() << "Directory junction creation unavailable: " << error.message();
    }

    // Remove the junction itself before MinGW's recursive fixture cleanup can follow it.
    struct Cleanup {
        const std::filesystem::path &path;
        ~Cleanup() {
            RemoveDirectoryW(path.c_str());
        }
    } cleanup{junction};

    for (const auto &root : {junction, junction / "", junction / "."}) {
        EXPECT_THROW((void)scan_media(std::array{root}), std::filesystem::filesystem_error);
    }

    ScanOptions options;
    options.max_files = 2;

    const auto result = scan_media(std::array{input}, options);

    ASSERT_EQ(result.files.size(), 1U);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_TRUE(std::filesystem::equivalent(result.files[0].path, input / "image.png"));
}
#endif

TEST(FingerprintCache, EachGenerationSettingChangesKeyAndInvalidSettingsFail) {
    const auto baseline = fingerprint_settings_key({});
    using Change = void (*)(FingerprintSettings &);
    const std::array<Change, 15> changes{
        [](auto &s) {
            s.image.hdr.exposure = 2;
        },
        [](auto &s) {
            s.image.hdr.peak_nits = 2000;
        },
        [](auto &s) {
            s.video.decode.hdr.exposure = 2;
        },
        [](auto &s) {
            s.video.decode.hdr.peak_nits = 2000;
        },
        [](auto &s) {
            s.kinds = MediaKinds::Images;
        },
        [](auto &s) {
            s.transforms = TransformPolicy::Dihedral;
        },
        [](auto &s) {
            --s.image.max_pixels;
        },
        [](auto &s) {
            --s.image.max_dimension;
        },
        [](auto &s) {
            --s.video.decode.max_pixels;
        },
        [](auto &s) {
            --s.video.decode.max_dimension;
        },
        [](auto &s) {
            s.video.decode.sample_interval = Timestamp{1};
        },
        [](auto &s) {
            s.video.prune_distance = PdqDistance{0};
        },
        [](auto &s) {
            s.video.prune_distance = PdqDistance{256};
        },
        [](auto &s) {
            --s.video.max_frames;
        },
        [](auto &s) {
            s.transforms = TransformPolicy::Rotations;
        }
    };

    for (auto change : changes) {
        FingerprintSettings s;
        change(s);

        EXPECT_NE(fingerprint_settings_key(s), baseline);
    }

    for (unsigned channel = 0; channel < 3; ++channel) {
        FingerprintSettings s;
        --s.image.alpha_background[channel];

        EXPECT_NE(fingerprint_settings_key(s), baseline);

        s = {};
        --s.video.decode.alpha_background[channel];

        EXPECT_NE(fingerprint_settings_key(s), baseline);
    }

    const std::array<Change, 9> invalid{
        [](auto &s) {
            s.image.hdr.exposure = 0;
        },
        [](auto &s) {
            s.image.hdr.peak_nits = std::numeric_limits<double>::infinity();
        },
        [](auto &s) {
            s.video.decode.hdr.exposure = std::numeric_limits<double>::quiet_NaN();
        },
        [](auto &s) {
            s.video.decode.hdr.peak_nits = 99;
        },
        [](auto &s) {
            s.kinds = static_cast<MediaKinds>(255);
        },
        [](auto &s) {
            s.transforms = static_cast<TransformPolicy>(255);
        },
        [](auto &s) {
            s.video.max_frames = 0;
        },
        [](auto &s) {
            s.image.max_pixels = 0;
        },
        [](auto &s) {
            s.video.decode.sample_interval = Timestamp{-1};
        }
    };

    for (auto change : invalid) {
        FingerprintSettings s;
        change(s);

        EXPECT_THROW((void)fingerprint_settings_key(s), std::invalid_argument);
    }
}

TEST(FingerprintCache, ReopenReplacementBudgetCorruptionAndKeyMismatchAreChecked) {
    TempDirectory directory;
    const auto path = directory.path() / u8"кеш";
    const FingerprintCache cache(path);
    const auto content = hash_blake3({}), settings = fingerprint_settings_key({});

    EXPECT_FALSE(cache.load(content, settings));

    cache.store(content, settings, image_value);

    EXPECT_EQ(FingerprintCache(path).load(content, settings), std::optional{image_value});

    cache.store(content, settings, video_value);

    EXPECT_EQ(cache.load(content, settings), std::optional{video_value});
    EXPECT_EQ(
        cache.load(content, settings, std::numeric_limits<std::size_t>::max()),
        std::optional{video_value}
    );
    EXPECT_THROW((void)cache.load(content, settings, 1), std::filesystem::filesystem_error);

    const auto entry = std::filesystem::directory_iterator(path)->path();
    write_file(entry, "broken");

    EXPECT_THROW((void)cache.load(content, settings), std::filesystem::filesystem_error);

    cache.store(content, settings, image_value);
    auto other_content = content;
    other_content.bytes[0] ^= 1;

    EXPECT_FALSE(cache.load(other_content, settings));

    cache.store(other_content, settings, video_value);

    for (const auto &file : std::filesystem::directory_iterator(path)) {
        if (file.path() != entry) {
            std::filesystem::copy_file(
                file.path(),
                entry,
                std::filesystem::copy_options::overwrite_existing
            );
        }
    }

    EXPECT_THROW((void)cache.load(content, settings), std::filesystem::filesystem_error);

    std::filesystem::remove(entry);
    std::filesystem::create_directory(entry);

    EXPECT_THROW((void)cache.load(content, settings), std::filesystem::filesystem_error);
    EXPECT_THROW((void)FingerprintCache({}), std::invalid_argument);
}

TEST(FingerprintCache, ConcurrentReadersSeeOnlyCompleteAtomicReplacements) {
    const TempDirectory directory;
    const FingerprintCache cache(directory.path());

    const auto content = hash_blake3({}), settings = fingerprint_settings_key({});
    cache.store(content, settings, image_value);

    std::barrier start(4);
    std::array<std::future<void>, 4> workers;

    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = std::async(std::launch::async, [&, i] {
            start.arrive_and_wait();

            for (unsigned iteration = 0; iteration < 20; ++iteration) {
                if (i < 2) {
                    cache.store(content, settings, i == 0 ? image_value : video_value);
                } else {
                    const auto value = cache.load(content, settings);

                    ASSERT_TRUE(value);
                    EXPECT_TRUE(*value == image_value || *value == video_value);
                }
            }
        });
    }

    for (auto &worker : workers) {
        EXPECT_NO_THROW(worker.get());
    }

    EXPECT_EQ(
        std::distance(
            std::filesystem::directory_iterator(directory.path()),
            std::filesystem::directory_iterator{}
        ),
        1
    );
}

TEST(Scanner, OverlappingRootsStableIdsByteDuplicatesAndParallelResultsAgree) {
    TempDirectory directory;
    const auto nested = directory.path() / "nested";

    std::filesystem::create_directory(nested);
    std::filesystem::copy_file(fixture("image/bridge.png"), directory.path() / "a.video");
    std::filesystem::copy_file(fixture("image/bridge.png"), nested / u8"б.png");
    std::filesystem::copy_file(fixture("video/vfr-alpha.mkv"), directory.path() / "c.image");

    write_file(directory.path() / "broken", "not media");
    write_file(nested / "same-broken", "not media");

    const std::array roots{directory.path(), nested, directory.path() / "a.video"};

    ScanOptions options;

    options.workers = 4;
    const auto parallel = scan_media(roots, options);

    options.workers = 1;
    const auto serial = scan_media(roots, options);

    ASSERT_EQ(parallel.files.size(), 5U);
    ASSERT_EQ(serial.files.size(), parallel.files.size());
    ASSERT_EQ(parallel.issues.size(), 2U);
    EXPECT_EQ(exact_duplicate_groups(parallel.files), exact_duplicate_groups(serial.files));
    EXPECT_EQ(exact_duplicate_groups(parallel.files).size(), 2U);

    for (std::size_t i = 0; i < parallel.files.size(); ++i) {
        const auto &a = parallel.files[i], &b = serial.files[i];

        EXPECT_EQ(a.id, MediaId{i});
        EXPECT_EQ(a.path, b.path);
        EXPECT_EQ(a.size, b.size);
        EXPECT_EQ(a.digest, b.digest);
        EXPECT_EQ(a.fingerprint, b.fingerprint);
        EXPECT_TRUE(a.digest);
    }

    const auto issue_keys = [](const ScanResult &result) {
        std::vector<std::pair<std::filesystem::path, ScanStage>> keys;

        for (const auto &issue : result.issues) {
            keys.emplace_back(issue.path, issue.stage);
        }

        std::ranges::sort(keys);

        return keys;
    };

    EXPECT_EQ(issue_keys(parallel), issue_keys(serial));
    EXPECT_EQ(
        std::ranges::count_if(
            parallel.files,
            [](const auto &file) {
                return file.fingerprint && std::holds_alternative<VpdqSignature>(*file.fingerprint);
            }
        ),
        1
    );
}

TEST(Scanner, CacheHitsFollowContentAndSettingsAndCorruptionIsRepaired) {
    const TempDirectory directory;
    const FingerprintCache cache(directory.path() / "cache");

    auto input = directory.path() / "image";
    std::filesystem::copy_file(fixture("image/bridge.png"), input);

    const std::array roots{directory.path()};
    const auto cold = scan_media(roots, {}, &cache);

    ASSERT_EQ(cold.files.size(), 1U);
    ASSERT_TRUE(cold.issues.empty());
    EXPECT_FALSE(cold.files[0].cache_hit);

    const auto warm = scan_media(roots, {}, &cache);

    ASSERT_EQ(warm.files.size(), 1U);
    EXPECT_TRUE(warm.files[0].cache_hit);
    EXPECT_EQ(warm.files[0].fingerprint, cold.files[0].fingerprint);

    std::filesystem::rename(input, directory.path() / "renamed");
    input = directory.path() / "renamed";

    EXPECT_TRUE(scan_media(roots, {}, &cache).files.at(0).cache_hit);

    ScanOptions options;
    options.fingerprints.transforms = TransformPolicy::Dihedral;
    const auto changed = scan_media(roots, options, &cache);

    ASSERT_EQ(changed.files.size(), 1U);
    ASSERT_TRUE(changed.files[0].fingerprint);
    EXPECT_FALSE(changed.files[0].cache_hit);
    EXPECT_EQ(std::get<ImageSignature>(*changed.files[0].fingerprint).variants.size(), 8U);

    for (const auto &entry : std::filesystem::directory_iterator(cache.directory())) {
        write_file(entry.path(), "corrupt");
    }

    const auto repaired = scan_media(roots, {}, &cache);

    ASSERT_EQ(repaired.issues.size(), 1U);
    EXPECT_EQ(repaired.issues[0].stage, ScanStage::CacheRead);
    ASSERT_EQ(repaired.files.size(), 1U);
    EXPECT_TRUE(repaired.files[0].fingerprint);
    EXPECT_TRUE(scan_media(roots, {}, &cache).files.at(0).cache_hit);

    std::filesystem::copy_file(
        fixture("image/pen.png"),
        input,
        std::filesystem::copy_options::overwrite_existing
    );
    const auto edited = scan_media(roots, {}, &cache);

    ASSERT_EQ(edited.files.size(), 1U);
    EXPECT_FALSE(edited.files[0].cache_hit);
    EXPECT_NE(edited.files[0].digest, cold.files[0].digest);
}

TEST(Scanner, CacheIdentityExcludesExplicitDescendantsAndPreservesSimilarNames) {
    const TempDirectory directory;
    const FingerprintCache cache(directory.path() / "Cache");

    const auto nested = cache.directory() / "nested";
    const auto hidden = nested / "hidden.png";
    const auto sibling = directory.path() / "Cache-other";

    std::filesystem::create_directory(nested);
    std::filesystem::copy_file(fixture("image/bridge.png"), hidden);
    std::filesystem::create_directory(sibling);
    std::filesystem::copy_file(fixture("image/bridge.png"), sibling / "image.png");
    const std::array roots{directory.path(), cache.directory(), nested, hidden};

    for (unsigned pass = 0; pass < 2; ++pass) {
        const auto result = scan_media(roots, {}, &cache);

        ASSERT_EQ(result.files.size(), 1U);
        EXPECT_TRUE(std::filesystem::equivalent(result.files[0].path, sibling / "image.png"));
        EXPECT_TRUE(result.issues.empty());
    }
}

TEST(Scanner, CacheIdentityHonorsFilesystemCaseSensitivity) {
    const TempDirectory directory;
    const auto upper = directory.path() / "Cache";
    const auto lower = directory.path() / "cache";

    std::filesystem::create_directory(upper);
    std::filesystem::copy_file(fixture("image/bridge.png"), directory.path() / "image.png");

    const bool insensitive = std::filesystem::exists(lower);
    const FingerprintCache cache(lower);

    if (!insensitive) {
        std::filesystem::copy_file(fixture("image/pen.png"), upper / "distinct.png");
    }

    const std::array roots{directory.path()};

    for (unsigned pass = 0; pass < 2; ++pass) {
        const auto result = scan_media(roots, {}, &cache);

        EXPECT_EQ(result.files.size(), insensitive ? 1U : 2U);
        EXPECT_TRUE(result.issues.empty());
    }

    const std::array cache_roots{lower};

    EXPECT_TRUE(scan_media(cache_roots, {}, &cache).files.empty());
}

TEST(Scanner, CacheWriteFailurePreservesFingerprintAndConcurrentScansPublishWholeEntries) {
    const TempDirectory directory;
    const auto input = directory.path() / "image";
    const FingerprintCache cache(directory.path() / "cache");

    std::filesystem::copy_file(fixture("image/bridge.png"), input);
    std::filesystem::remove(cache.directory());

    const std::array roots{input};
    write_file(cache.directory(), "not a directory");

    const auto failed = scan_media(roots, {}, &cache);

    ASSERT_EQ(failed.files.size(), 1U);
    ASSERT_FALSE(failed.issues.empty());
    EXPECT_EQ(failed.issues.back().stage, ScanStage::CacheWrite);
    EXPECT_TRUE(failed.files[0].digest);
    EXPECT_TRUE(failed.files[0].fingerprint);

    std::filesystem::remove(cache.directory());
    std::filesystem::create_directory(cache.directory());
    std::array<std::future<ScanResult>, 4> workers;

    for (auto &worker : workers) {
        worker = std::async(std::launch::async, [&] {
            return scan_media(roots, {}, &cache);
        });
    }

    for (auto &worker : workers) {
        const auto result = worker.get();

        ASSERT_EQ(result.files.size(), 1U);
        EXPECT_TRUE(result.issues.empty());
        EXPECT_EQ(result.files[0].fingerprint, failed.files[0].fingerprint);
    }

    EXPECT_TRUE(scan_media(roots, {}, &cache).files.at(0).cache_hit);
}

TEST(Scanner, InvalidRootsConfigurationBudgetsAndCancellationAbort) {
    TempDirectory directory;
    const std::array roots{directory.path()};

    EXPECT_TRUE(scan_media(roots).files.empty());
    EXPECT_THROW((void)scan_media({}), std::invalid_argument);

    for (unsigned workers : {0U, 257U}) {
        ScanOptions options;
        options.workers = workers;

        EXPECT_THROW((void)scan_media(roots, options), std::invalid_argument);
    }

    ScanOptions options;
    options.fingerprints.image.max_pixels = 0;

    EXPECT_THROW((void)scan_media(roots, options), std::invalid_argument);
    EXPECT_THROW(
        (void)scan_media(std::array{directory.path() / "missing"}),
        std::filesystem::filesystem_error
    );

    write_file(directory.path() / "a", "a");
    write_file(directory.path() / "b", "b");
    options = {};
    options.max_files = 1;

    EXPECT_THROW((void)scan_media(roots, options), std::length_error);

    std::stop_source stop;
    stop.request_stop();

    try {
        (void)scan_media(roots, {}, nullptr, stop.get_token());
        FAIL();
    } catch (const std::system_error &error) {
        EXPECT_EQ(error.code(), std::errc::operation_canceled);
    }
}

TEST(Scanner, FileSymlinksAreDistinctButDirectoryCyclesAreNotTraversed) {
    const TempDirectory directory;
    const auto image = directory.path() / "image";

    std::error_code error;
    std::filesystem::copy_file(fixture("image/bridge.png"), image);
    std::filesystem::create_symlink(image, directory.path() / "alias", error);

    if (error) {
        GTEST_SKIP() << "Symlink creation unavailable: " << error.message();
    }

    std::filesystem::create_directory_symlink(directory.path(), directory.path() / "cycle", error);

    ASSERT_FALSE(error);

    const auto result = scan_media(std::array{directory.path()});

    EXPECT_EQ(result.files.size(), 2U);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(exact_duplicate_groups(result.files).size(), 1U);
}

TEST(Scanner, ParentComponentsAfterDirectorySymlinksResolveBeforeNormalization) {
    const TempDirectory directory;
    const auto target = directory.path() / "target";
    const auto alias = directory.path() / "alias";

    std::error_code error;
    std::filesystem::create_directories(target / "nested");
    std::filesystem::copy_file(fixture("image/bridge.png"), target / "image.png");
    std::filesystem::copy_file(fixture("image/pen.png"), directory.path() / "image.png");
    std::filesystem::create_directory_symlink(target / "nested", alias, error);

    if (error) {
        GTEST_SKIP() << "Symlink creation unavailable: " << error.message();
    }

    const auto input = alias / ".." / "image.png";
    const auto result = scan_media(std::array{input});

    ASSERT_EQ(result.files.size(), 1U);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(result.files[0].digest, hash_blake3_file(input));

    const auto parent = std::filesystem::canonical(alias / "..");
    const FingerprintCache cache(alias / ".." / "cache");
    EXPECT_TRUE(std::filesystem::equivalent(cache.directory(), parent / "cache"));

    if (parent == target) {
        EXPECT_FALSE(std::filesystem::exists(directory.path() / "cache"));
    }
}

TEST(Scanner, NormalizationDoesNotBypassMissingParentComponents) {
    const TempDirectory directory;
    const auto image = directory.path() / "image.png";
    const auto input = directory.path() / "missing" / ".." / "image.png";

    std::error_code error;
    std::filesystem::copy_file(fixture("image/bridge.png"), image);

    const auto status = std::filesystem::status(input, error);

    if (std::filesystem::is_regular_file(status)) {
        GTEST_SKIP() << "Filesystem resolves parent components before checking existence";
    }

    EXPECT_THROW((void)scan_media(std::array{input}), std::filesystem::filesystem_error);
}

TEST(Scanner, DirectorySymlinkRootsWithTrailingSeparatorsAreRejected) {
    const TempDirectory directory;
    const auto alias = directory.path() / "alias";

    std::error_code error;
    std::filesystem::create_directory_symlink(directory.path(), alias, error);

    if (error) {
        GTEST_SKIP() << "Symlink creation unavailable: " << error.message();
    }

    for (const auto &root : {alias, alias / "", alias / "."}) {
        EXPECT_THROW((void)scan_media(std::array{root}), std::filesystem::filesystem_error);
    }
}

}
