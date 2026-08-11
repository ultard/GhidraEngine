#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "ghidraengine/error.hpp"
#include "ghidraengine/types.hpp"

namespace ghidraengine::platform {

#if defined(_WIN32)
using NativeHandle = void*;
inline constexpr NativeHandle kInvalidHandle = nullptr;
#else
using NativeHandle = int;
inline constexpr NativeHandle kInvalidHandle = -1;
#endif

class File {
public:
    File() = default;
    ~File();

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    static Result<File> open_read(const std::filesystem::path& path, bool sequential);

    [[nodiscard]] Result<std::size_t> read_at(std::uint64_t offset,
                                              std::span<std::uint8_t> out) const;

    [[nodiscard]] Result<std::uint64_t> size() const;
    [[nodiscard]] Result<FileIdentity> identity() const;

    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }
    [[nodiscard]] NativeHandle native() const noexcept { return handle_; }

    void close() noexcept;

private:
    NativeHandle handle_ = kInvalidHandle;
};

struct DirEntry {
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    bool is_directory = false;
    bool is_symlink = false;
    bool is_hidden = false;
    bool has_metadata = false;
};

Result<void> list_directory(const std::filesystem::path& dir, std::vector<DirEntry>& out);

Result<void> stat_entry(const std::filesystem::path& path, DirEntry& entry);

Result<FileIdentity> identity_of(const std::filesystem::path& path);

bool is_rotational_storage(const std::filesystem::path& path);

std::string to_utf8(const std::filesystem::path& path);
std::filesystem::path from_utf8(std::string_view utf8);

Error last_error(std::string_view context);

}
