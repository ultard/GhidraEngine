#pragma once

#include <string_view>

namespace ghidraengine {

// `*` (no separator), `**` (any run), `?`, and `[abc]` / `[a-z]` / `[!abc]`.
// Case-insensitive on Windows; `/` and `\` are equivalent.
bool glob_match(std::string_view pattern, std::string_view text) noexcept;

} // namespace ghidraengine
