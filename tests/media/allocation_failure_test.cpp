#include <GhidraEngine/index/mih_pdq.hpp>
#include <GhidraEngine/index/video.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {

constexpr auto unlimited = std::numeric_limits<std::size_t>::max();
thread_local std::size_t allocations_left = unlimited;

} // namespace

void *operator new(std::size_t bytes) {
    if (allocations_left != unlimited) {
        if (allocations_left == 0) {
            allocations_left = unlimited;
            throw std::bad_alloc();
        }

        --allocations_left;
    }

    if (auto *memory = std::malloc(std::max(bytes, std::size_t{1}))) {
        return memory;
    }

    throw std::bad_alloc();
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

int main() try {
    using namespace GhidraEngine;
    const std::array entries{
        PdqIndexEntry{FingerprintId{10}, {}},
        PdqIndexEntry{FingerprintId{20}, {}},
        PdqIndexEntry{FingerprintId{30}, {}}
    };
    const MihPdqIndex ordered(entries);
    auto extended = ordered;
    extended.insert(FingerprintId{0}, {});
    const auto original_hits = ordered.search_within({}, 256);
    const auto extended_hits = extended.search_within({}, 256);
    unsigned insertion_failures = 0;
    bool insertion_completed = false;

    for (unsigned failure = 0; failure < 32; ++failure) {
        auto index = ordered;
        allocations_left = failure;

        try {
            index.insert(FingerprintId{0}, {});
            insertion_completed = true;
        } catch (const std::bad_alloc &) {
            ++insertion_failures;
        }

        allocations_left = unlimited;
        const auto &expected = insertion_completed ? extended_hits : original_hits;

        if (index.search_within({}, 256) != expected) {
            std::cerr << "MIH insertion lost records at allocation " << failure << '\n';

            return 1;
        }

        if (insertion_completed) {
            break;
        }

        index.insert(FingerprintId{0}, {});

        if (index.search_within({}, 256) != extended_hits) {
            std::cerr << "MIH insertion could not recover after allocation failure\n";

            return 1;
        }

        bool duplicate_rejected = false;

        try {
            index.insert(FingerprintId{20}, {});
        } catch (const std::invalid_argument &) {
            duplicate_rejected = true;
        }

        if (!duplicate_rejected) {
            std::cerr << "MIH lost duplicate ID tracking after allocation failure\n";

            return 1;
        }
    }

    if (!insertion_completed || insertion_failures == 0) {
        std::cerr << "MIH insertion did not exercise allocation failures and recovery\n";

        return 1;
    }

    const std::array source_records{
        VideoSignatureRecord{VideoId{1}, {{{{}, PdqQuality{100}, Timestamp{-1}}}}},
        VideoSignatureRecord{VideoId{2}, {{{PdqHash{{7, 0, 0, 0}}, PdqQuality{50}, Timestamp{2}}}}}
    };
    const std::array target_records{
        VideoSignatureRecord{VideoId{9}, {{{PdqHash{{9, 0, 0, 0}}, PdqQuality{80}, Timestamp{9}}}}}
    };
    const VideoFingerprintCatalog source(source_records), before(target_records);
    unsigned failures = 0;

    for (unsigned failure = 0; failure < 32; ++failure) {
        VideoFingerprintCatalog target(before);
        allocations_left = failure;
        bool success = false;

        try {
            target = source;
            success = true;
        } catch (const std::bad_alloc &) {
            ++failures;
        }

        allocations_left = unlimited;
        const auto &expected = success ? source : before;

        if (!std::ranges::equal(target.records(), expected.records()) ||
            !std::ranges::equal(target.frames(), expected.frames()) ||
            target.index_entries() != expected.index_entries()) {
            std::cerr << "Catalog assignment lost its transactional guarantee at allocation "
                      << failure << '\n';
            return 1;
        }

        if (success) {
            return failures > 0 ? 0 : 1;
        }
    }

    std::cerr << "Assignment never completed within allocation budget\n";

    return 1;
} catch (const std::exception &error) {
    allocations_left = unlimited;
    std::cerr << error.what() << '\n';

    return 1;
}
