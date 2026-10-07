#include <GhidraEngine/version.hpp>

namespace GhidraEngine {

std::string_view version() noexcept {
    return GHIDRAENGINE_VERSION;
}

}
