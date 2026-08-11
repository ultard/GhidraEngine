// No exception escapes a scan: per-file problems land in Report::errors, while
// operations that can fail as a whole return Result<T>.
#pragma once

#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace ghidraengine {

enum class ErrorCode {
    Ok = 0,
    NotFound,          // path does not exist
    AccessDenied,      // permissions, locked file, sharing violation
    IoError,           // read failed mid-stream
    UnsupportedFormat, // magic bytes matched nothing we can decode
    DecodeFailed,      // decoder rejected the payload
    CorruptFile,       // truncated or structurally invalid
    Cancelled,         // stop_token was signalled
    CacheError,        // persistent cache could not be opened or written
    InvalidArgument,   // caller passed a nonsensical config
    OutOfMemory,
    Unknown,
};

const char* to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
    explicit Error(ErrorCode c) : code(c), message(to_string(c)) {}
};

// Not std::expected: C++20 toolchains predate <expected>.
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}       // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {}   // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { return std::get<0>(storage_); }
    const T& value() const& { return std::get<0>(storage_); }
    T&& value() && { return std::get<0>(std::move(storage_)); }

    T* operator->() { return &std::get<0>(storage_); }
    const T* operator->() const { return &std::get<0>(storage_); }
    T& operator*() & { return std::get<0>(storage_); }
    const T& operator*() const& { return std::get<0>(storage_); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
    [[nodiscard]] Error&& error() && { return std::get<1>(std::move(storage_)); }

    template <typename U>
    T value_or(U&& fallback) const& {
        return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), failed_(true) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    Error error_;
    bool failed_ = false;
};

} // namespace ghidraengine
