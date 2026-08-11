#include "core/platform.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ghidraengine::platform {
namespace {

constexpr std::int64_t kFiletimeEpochOffset = 116444736000000000LL;

std::int64_t filetime_to_unix_ns(const FILETIME& ft) {
    const std::int64_t ticks =
        (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (ticks - kFiletimeEpochOffset) * 100;
}

std::uint64_t combine(DWORD high, DWORD low) {
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

std::wstring extended_path(const std::filesystem::path& path) {
    std::wstring native = path.native();
    if (native.size() < MAX_PATH) {
        return native;
    }
    if (native.rfind(LR"(\\?\)", 0) == 0 || native.rfind(LR"(\\.\)", 0) == 0) {
        return native;
    }
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return native;
    }
    std::wstring result = absolute.make_preferred().native();
    if (result.rfind(LR"(\\)", 0) == 0) {
        return LR"(\\?\UNC\)" + result.substr(2);
    }
    return LR"(\\?\)" + result;
}

ErrorCode classify(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
            return ErrorCode::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return ErrorCode::AccessDenied;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ErrorCode::OutOfMemory;
        default:
            return ErrorCode::IoError;
    }
}

std::string message_for(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return "Windows error " + std::to_string(code);
    }
    std::wstring wide(buffer, length);
    ::LocalFree(buffer);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n')) {
        wide.pop_back();
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                            static_cast<int>(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

}

Error last_error(std::string_view context) {
    const DWORD code = ::GetLastError();
    return Error{classify(code), std::string(context) + ": " + message_for(code)};
}

File::~File() { close(); }

File::File(File&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidHandle;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalidHandle;
    }
    return *this;
}

void File::close() noexcept {
    if (handle_ != kInvalidHandle) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = kInvalidHandle;
    }
}

Result<File> File::open_read(const std::filesystem::path& path, bool sequential) {
    const DWORD flags = sequential ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_FLAG_RANDOM_ACCESS;
    HANDLE handle = ::CreateFileW(
        extended_path(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return last_error("open " + to_utf8(path));
    }
    File file;
    file.handle_ = handle;
    return Result<File>{std::move(file)};
}

Result<std::size_t> File::read_at(std::uint64_t offset, std::span<std::uint8_t> out) const {
    if (!valid()) {
        return Error{ErrorCode::IoError, "read from a closed handle"};
    }
    std::size_t total = 0;
    while (total < out.size()) {
        const std::size_t remaining = out.size() - total;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, 32u * 1024u * 1024u));

        OVERLAPPED overlapped{};
        const std::uint64_t position = offset + total;
        overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);

        DWORD read = 0;
        if (!::ReadFile(static_cast<HANDLE>(handle_), out.data() + total, chunk, &read,
                        &overlapped)) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_HANDLE_EOF) {
                break;
            }
            return last_error("read");
        }
        if (read == 0) {
            break;
        }
        total += read;
    }
    return total;
}

Result<std::uint64_t> File::size() const {
    LARGE_INTEGER value{};
    if (!::GetFileSizeEx(static_cast<HANDLE>(handle_), &value)) {
        return last_error("stat size");
    }
    return static_cast<std::uint64_t>(value.QuadPart);
}

Result<FileIdentity> File::identity() const {
    FILE_ID_INFO info{};
    if (::GetFileInformationByHandleEx(static_cast<HANDLE>(handle_), FileIdInfo, &info,
                                       sizeof(info))) {
        FileIdentity identity;
        identity.volume = info.VolumeSerialNumber;
        std::memcpy(&identity.id_low, info.FileId.Identifier, 8);
        std::memcpy(&identity.id_high, info.FileId.Identifier + 8, 8);
        return identity;
    }

    BY_HANDLE_FILE_INFORMATION legacy{};
    if (!::GetFileInformationByHandle(static_cast<HANDLE>(handle_), &legacy)) {
        return last_error("stat identity");
    }
    FileIdentity identity;
    identity.volume = legacy.dwVolumeSerialNumber;
    identity.id_low = combine(legacy.nFileIndexHigh, legacy.nFileIndexLow);
    identity.id_high = 0;
    return identity;
}

Result<void> list_directory(const std::filesystem::path& dir, std::vector<DirEntry>& out) {
    out.clear();

    std::wstring pattern = extended_path(dir);
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');

    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                     FindExSearchNameMatch, nullptr,
                                     FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            return {};
        }
        return last_error("list " + to_utf8(dir));
    }

    do {
        const std::wstring_view name{data.cFileName};
        if (name == L"." || name == L"..") {
            continue;
        }

        DirEntry entry;
        entry.path = dir / std::filesystem::path(name);
        entry.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.is_symlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry.is_hidden = (data.dwFileAttributes &
                           (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
        entry.size = combine(data.nFileSizeHigh, data.nFileSizeLow);
        entry.mtime_ns = filetime_to_unix_ns(data.ftLastWriteTime);
        entry.has_metadata = true;
        out.push_back(std::move(entry));
    } while (::FindNextFileW(find, &data));

    const DWORD code = ::GetLastError();
    ::FindClose(find);
    if (code != ERROR_NO_MORE_FILES) {
        ::SetLastError(code);
        return last_error("list " + to_utf8(dir));
    }
    return {};
}

Result<void> stat_entry(const std::filesystem::path& path, DirEntry& entry) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(extended_path(path).c_str(), GetFileExInfoStandard, &data)) {
        return last_error("stat " + to_utf8(path));
    }
    entry.size = combine(data.nFileSizeHigh, data.nFileSizeLow);
    entry.mtime_ns = filetime_to_unix_ns(data.ftLastWriteTime);
    entry.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.is_symlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry.is_hidden = (data.dwFileAttributes &
                       (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
    entry.has_metadata = true;
    return {};
}

Result<FileIdentity> identity_of(const std::filesystem::path& path) {
    HANDLE handle = ::CreateFileW(
        extended_path(path).c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return last_error("identify " + to_utf8(path));
    }
    struct HandleGuard {
        HANDLE h;
        ~HandleGuard() { ::CloseHandle(h); }
    } guard{handle};

    FILE_ID_INFO info{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        FileIdentity identity;
        identity.volume = info.VolumeSerialNumber;
        std::memcpy(&identity.id_low, info.FileId.Identifier, 8);
        std::memcpy(&identity.id_high, info.FileId.Identifier + 8, 8);
        return identity;
    }

    BY_HANDLE_FILE_INFORMATION legacy{};
    if (!::GetFileInformationByHandle(handle, &legacy)) {
        return last_error("identify " + to_utf8(path));
    }
    FileIdentity identity;
    identity.volume = legacy.dwVolumeSerialNumber;
    identity.id_low = combine(legacy.nFileIndexHigh, legacy.nFileIndexLow);
    return identity;
}

bool is_rotational_storage(const std::filesystem::path& path) {
    static std::mutex mutex;
    static std::unordered_map<std::wstring, bool> cache;

    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return false;
    }
    const std::wstring root = absolute.root_name().native();
    if (root.empty()) {
        return false;
    }

    {
        const std::lock_guard lock(mutex);
        if (const auto it = cache.find(root); it != cache.end()) {
            return it->second;
        }
    }

    bool rotational = false;
    const std::wstring device = LR"(\\.\)" + root;
    HANDLE handle = ::CreateFileW(device.c_str(), 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
        DWORD returned = 0;
        if (::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                              &descriptor, sizeof(descriptor), &returned, nullptr)) {
            rotational = descriptor.IncursSeekPenalty != FALSE;
        }
        ::CloseHandle(handle);
    }

    const std::lock_guard lock(mutex);
    cache.emplace(root, rotational);
    return rotational;
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::wstring& wide = path.native();
    if (wide.empty()) {
        return {};
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                            static_cast<int>(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

std::filesystem::path from_utf8(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int chars = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                            static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(chars), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), chars);
    return std::filesystem::path(std::move(wide));
}

}

#endif
