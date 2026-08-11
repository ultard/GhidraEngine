#include "core/glob.hpp"

#include <cctype>

namespace ghidraengine {
namespace {

constexpr bool kCaseInsensitive =
#ifdef _WIN32
    true;
#else
    false;
#endif

char normalize(char c) noexcept {
    if (c == '\\') {
        return '/';
    }
    if (kCaseInsensitive) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return c;
}

bool match_class(std::string_view pattern, std::size_t& index, char c) noexcept {
    ++index;
    bool negated = false;
    if (index < pattern.size() && (pattern[index] == '!' || pattern[index] == '^')) {
        negated = true;
        ++index;
    }

    bool matched = false;
    bool first = true;
    while (index < pattern.size() && (pattern[index] != ']' || first)) {
        first = false;
        const char low = normalize(pattern[index]);
        if (index + 2 < pattern.size() && pattern[index + 1] == '-' &&
            pattern[index + 2] != ']') {
            const char high = normalize(pattern[index + 2]);
            if (c >= low && c <= high) {
                matched = true;
            }
            index += 3;
        } else {
            if (c == low) {
                matched = true;
            }
            ++index;
        }
    }
    if (index < pattern.size()) {
        ++index;
    }
    return matched != negated;
}

} // namespace

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
    constexpr std::size_t npos = std::string_view::npos;

    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star_p = npos;
    std::size_t star_t = 0;
    std::size_t dstar_p = npos;
    std::size_t dstar_t = 0;

    while (t < text.size()) {
        if (p < pattern.size()) {
            const char pc = pattern[p];

            if (pc == '*') {
                if (p + 1 < pattern.size() && pattern[p + 1] == '*') {
                    p += 2;
                    if (p < pattern.size() && (pattern[p] == '/' || pattern[p] == '\\')) {
                        ++p;
                    }
                    dstar_p = p;
                    dstar_t = t;
                    star_p = npos;
                } else {
                    ++p;
                    star_p = p;
                    star_t = t;
                }
                continue;
            }
            if (pc == '?') {
                if (normalize(text[t]) != '/') {
                    ++p;
                    ++t;
                    continue;
                }
            } else if (pc == '[') {
                std::size_t probe = p;
                if (match_class(pattern, probe, normalize(text[t]))) {
                    p = probe;
                    ++t;
                    continue;
                }
            } else if (normalize(pc) == normalize(text[t])) {
                ++p;
                ++t;
                continue;
            }
        }

        if (star_p != npos && normalize(text[star_t]) != '/') {
            ++star_t;
            t = star_t;
            p = star_p;
            continue;
        }

        star_p = npos;
        if (dstar_p != npos && dstar_t < text.size()) {
            ++dstar_t;
            t = dstar_t;
            p = dstar_p;
            continue;
        }
        return false;
    }

    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

} // namespace ghidraengine
