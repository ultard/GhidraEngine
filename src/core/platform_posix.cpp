#include "core/platform.hpp"

#ifndef _WIN32

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sysmacros.h>
#include <fstream>
#endif

namespace ghidraengine::platform {
namespace {

ErrorCode classify(int code) {
    switch (code) {
        case ENOENT:
        case ENOTDIR:
        case ENAMETOOLONG:
            return ErrorCode::NotFound;
        case EACCES:
        case EPERM:
            return ErrorCode::AccessDenied;
        case ENOMEM:
            return ErrorCode::OutOfMemory;
        default:
            return ErrorCode::IoError;
    }
}

std::int64_t timespec_to_ns(const struct timespec& ts) {
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

#if defined(__APPLE__)
#define GHIDRAENGINE_MTIM(st) ((st).st_mtimespec)
#else
#define GHIDRAENGINE_MTIM(st) ((st).st_mtim)
#endif

} // namespace

Error last_error(std::string_view context) {
    const int code = errno;
    return Error{classify(code), std::string(context) + ": " + std::strerror(code)};
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
        ::close(handle_);
        handle_ = kInvalidHandle;
    }
}

Result<File> File::open_read(const std::filesystem::path& path, bool sequential) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        return last_error("open " + to_utf8(path));
    }

#if defined(POSIX_FADV_SEQUENTIAL)
    // Tell the kernel how we intend to read so read-ahead is sized correctly.
    ::posix_fadvise(fd, 0, 0, sequential ? POSIX_FADV_SEQUENTIAL : POSIX_FADV_RANDOM);
#elif defined(F_RDAHEAD)
    ::fcntl(fd, F_RDAHEAD, sequential ? 1 : 0); // macOS
#else
    (void)sequential;
#endif

    File file;
    file.handle_ = fd;
    return Result<File>{std::move(file)};
}

Result<std::size_t> File::read_at(std::uint64_t offset, std::span<std::uint8_t> out) const {
    if (!valid()) {
        return Error{ErrorCode::IoError, "read from a closed handle"};
    }
    std::size_t total = 0;
    while (total < out.size()) {
        const ssize_t read = ::pread(handle_, out.data() + total, out.size() - total,
                                     static_cast<off_t>(offset + total));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return last_error("read");
        }
        if (read == 0) {
            break; // end of file
        }
        total += static_cast<std::size_t>(read);
    }
    return total;
}

Result<std::uint64_t> File::size() const {
    struct stat info {};
    if (::fstat(handle_, &info) != 0) {
        return last_error("stat size");
    }
    return static_cast<std::uint64_t>(info.st_size);
}

Result<FileIdentity> File::identity() const {
    struct stat info {};
    if (::fstat(handle_, &info) != 0) {
        return last_error("stat identity");
    }
    FileIdentity identity;
    identity.volume = static_cast<std::uint64_t>(info.st_dev);
    identity.id_low = static_cast<std::uint64_t>(info.st_ino);
    return identity;
}

Result<void> list_directory(const std::filesystem::path& dir, std::vector<DirEntry>& out) {
    out.clear();

    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
        return last_error("list " + to_utf8(dir));
    }

    errno = 0;
    while (const struct dirent* entry = ::readdir(handle)) {
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..") {
            continue;
        }

        DirEntry item;
        item.path = dir / name;
        item.is_hidden = name.front() == '.';

        // d_type avoids a stat per entry; DT_UNKNOWN (older XFS, some network
        // mounts) falls back to lstat.
#ifdef DT_DIR // glibc, macOS and the BSDs; absent only on exotic libcs
        switch (entry->d_type) {
            case DT_DIR:
                item.is_directory = true;
                break;
            case DT_LNK:
                item.is_symlink = true;
                break;
            case DT_REG:
                break;
            case DT_UNKNOWN:
            default: {
                struct stat info {};
                if (::lstat(item.path.c_str(), &info) == 0) {
                    item.is_directory = S_ISDIR(info.st_mode);
                    item.is_symlink = S_ISLNK(info.st_mode);
                    item.size = static_cast<std::uint64_t>(info.st_size);
                    item.mtime_ns = timespec_to_ns(GHIDRAENGINE_MTIM(info));
                    item.has_metadata = !item.is_symlink;
                }
                break;
            }
        }
#else
        struct stat info {};
        if (::lstat(item.path.c_str(), &info) == 0) {
            item.is_directory = S_ISDIR(info.st_mode);
            item.is_symlink = S_ISLNK(info.st_mode);
            item.size = static_cast<std::uint64_t>(info.st_size);
            item.mtime_ns = timespec_to_ns(GHIDRAENGINE_MTIM(info));
            item.has_metadata = !item.is_symlink;
        }
#endif
        out.push_back(std::move(item));
        errno = 0;
    }

    const int code = errno;
    ::closedir(handle);
    if (code != 0) {
        errno = code;
        return last_error("list " + to_utf8(dir));
    }
    return {};
}

Result<void> stat_entry(const std::filesystem::path& path, DirEntry& entry) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return last_error("stat " + to_utf8(path));
    }
    entry.size = static_cast<std::uint64_t>(info.st_size);
    entry.mtime_ns = timespec_to_ns(GHIDRAENGINE_MTIM(info));
    entry.is_directory = S_ISDIR(info.st_mode);
    entry.has_metadata = true;
    return {};
}

Result<FileIdentity> identity_of(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return last_error("identify " + to_utf8(path));
    }
    FileIdentity identity;
    identity.volume = static_cast<std::uint64_t>(info.st_dev);
    identity.id_low = static_cast<std::uint64_t>(info.st_ino);
    return identity;
}

bool is_rotational_storage(const std::filesystem::path& path) {
#if defined(__linux__)
    // The kernel's own answer, cached per device: it costs a stat plus a read.
    static std::mutex mutex;
    static std::unordered_map<dev_t, bool> cache;

    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    const dev_t device = info.st_dev;

    {
        const std::lock_guard lock(mutex);
        if (const auto it = cache.find(device); it != cache.end()) {
            return it->second;
        }
    }

    bool rotational = false;
    const unsigned major_number = ::major(device);
    const unsigned minor_number = ::minor(device);

    // Partitions carry their parent disk's flag (/dev/sda3 -> /dev/sda).
    for (const std::string candidate :
         {"/sys/dev/block/" + std::to_string(major_number) + ":" +
              std::to_string(minor_number) + "/queue/rotational",
          "/sys/dev/block/" + std::to_string(major_number) + ":" +
              std::to_string(minor_number) + "/../queue/rotational"}) {
        std::ifstream stream(candidate);
        int value = 0;
        if (stream >> value) {
            rotational = value != 0;
            break;
        }
    }

    const std::lock_guard lock(mutex);
    cache.emplace(device, rotational);
    return rotational;
#else
    (void)path;
    return false; // macOS and BSDs: assume flash storage
#endif
}

std::string to_utf8(const std::filesystem::path& path) {
    return path.native(); // POSIX paths are already byte strings
}

std::filesystem::path from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::string(utf8));
}

} // namespace ghidraengine::platform

#endif // !_WIN32
