#include "core/file_io.hpp"

namespace ghidraengine {

Result<std::size_t> read_header(const platform::File& file,
                                std::span<std::uint8_t, kHeaderBytes> out) {
    auto read = file.read_at(0, out);
    if (!read) {
        return read.error();
    }
    return read.value();
}

Result<void> read_entire_file(const std::filesystem::path& path,
                              std::vector<std::uint8_t>& buffer,
                              std::uint64_t max_size) {
    auto file = platform::File::open_read(path, true);
    if (!file) {
        return file.error();
    }

    auto size = file->size();
    if (!size) {
        return size.error();
    }
    if (max_size != 0 && size.value() > max_size) {
        return Error{ErrorCode::UnsupportedFormat, "file exceeds the decode size limit"};
    }
    if (size.value() == 0) {
        buffer.clear();
        return Error{ErrorCode::CorruptFile, "file is empty"};
    }

    buffer.resize(size.value());
    auto read = file->read_at(0, buffer);
    if (!read) {
        return read.error();
    }
    if (read.value() != buffer.size()) {
        buffer.resize(read.value());
    }
    return {};
}

}
