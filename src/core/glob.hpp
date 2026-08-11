#pragma once

#include <string_view>

namespace ghidraengine {

bool glob_match(std::string_view pattern, std::string_view text) noexcept;

}
