#ifndef GHIDRAENGINE_TESTS_SUPPORT_TEMP_DIRECTORY_HPP
#define GHIDRAENGINE_TESTS_SUPPORT_TEMP_DIRECTORY_HPP

#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace GhidraEngine::test_support {

// Exclusive creation never overwrites existing fixtures.
class TempDirectory {
public:
    TempDirectory() {
        std::random_device random;
        const auto parent = std::filesystem::temp_directory_path();

        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            auto candidate = parent / ("GhidraEngine-fixture-" + std::to_string(random()) + "-" +
                                       std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                path_ = std::move(candidate);

                return;
            }
        }

        throw std::runtime_error("Cannot create a unique GhidraEngine fixture directory");
    }

    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);

        if (error) {
            std::cerr << "Cannot remove fixture directory " << path_ << ": " << error.message()
                      << '\n';
        }
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace GhidraEngine::test_support

#endif
