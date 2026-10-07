#include <GhidraEngine/cache/fingerprint.hpp>

#include "../support/temp_directory.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>

namespace {

constexpr auto unlimited = std::numeric_limits<std::size_t>::max();
thread_local std::size_t allocations_left = unlimited;
thread_local bool failure_injected = false;

}

void *operator new(std::size_t bytes) {
    if (allocations_left != unlimited) {
        if (allocations_left == 0) {
            allocations_left = unlimited;
            failure_injected = true;
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
    std::set_terminate([] {
        std::_Exit(73);
    });
    using namespace GhidraEngine;
    test_support::TempDirectory directory;
    const FingerprintCache cache(directory.path());
    const MediaFingerprint previous = ImageSignature{{{}}};
    const MediaFingerprint replacement = ImageSignature{{{PdqHash{{1, 2, 3, 4}}, PdqQuality{50}}}};
    for (unsigned budget = 0; budget < 128; ++budget) {
        cache.store({}, {}, previous);
        failure_injected = false;
        allocations_left = budget;
        bool completed = false;

        try {
            cache.store({}, {}, replacement);
            completed = true;
        } catch (const std::bad_alloc &) {
            // The injector resumes normal allocation after its single failure.
        }

        allocations_left = unlimited;
        const auto actual = cache.load({}, {});

        if (!actual ||
            (completed ? *actual != replacement : (*actual != previous && *actual != replacement))) {
            std::cerr << "Cache replacement lost data at allocation " << budget << '\n';

            return 1;
        }

        unsigned entries = 0;

        for (const auto &entry : std::filesystem::directory_iterator(directory.path())) {
            if (!entry.is_regular_file()) {
                std::cerr << "Cache left a staging directory at allocation " << budget << '\n';

                return 1;
            }

            ++entries;
        }

        if (entries != 1) {
            std::cerr << "Cache left unexpected entries at allocation " << budget << '\n';

            return 1;
        }

        // A swallowed allocation failure must not end the sweep early.
        if (!failure_injected) {
            return completed && budget > 0 ? 0 : 1;
        }
    }

    std::cerr << "Cache replacement never completed within allocation budget\n";

    return 1;
} catch (const std::exception &error) {
    allocations_left = unlimited;
    std::cerr << error.what() << '\n';

    return 1;
}
