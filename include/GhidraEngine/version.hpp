#ifndef GHIDRAENGINE_VERSION_HPP
#define GHIDRAENGINE_VERSION_HPP

#include <GhidraEngine/export.hpp>

#include <string_view>

namespace GhidraEngine {

[[nodiscard]] GHIDRAENGINE_EXPORT std::string_view version() noexcept;

}

#endif
