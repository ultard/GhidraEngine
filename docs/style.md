# Style guide

These rules cover maintained C++, CMake and Conan recipe build files, including tests and
benchmarks. Exclude generated files, build trees, third-party code and vendored scripts such
as `conan_provider.cmake`.

Use maintained headers and implementations as examples. For CMake, follow the root
`CMakeLists.txt` and `cmake/Install.cmake`. Compact tests and benchmark commands do not
override these rules.

## Common rules

- Indent with four spaces, never tabs. Aim for 100 columns without splitting paths,
  generator expressions or expressions that read better intact.
- Separate logical steps with one blank line. Keep related statements together;
  do not add repeated blank lines or align unrelated declarations into columns.
- Comments explain reasons, constraints, lifetime and platform behavior. Avoid narration
  of obvious code and decorative separators.
- Format touched code only unless the task requests a broader pass. Preserve behavior,
  conditions, argument order, quoting, list expansion and benchmark timed-loop contents.

## C++

### Blocks and whitespace

Put opening braces on the declaration or control-flow line and closing braces on their own
line. Always use braces for control flow. Use `} else {` and `} catch (...) {`.
Function bodies stay multiline, including short accessors.

Do not indent namespace contents. Leave one blank line after the opening namespace brace
and before the closing brace. Align `public:` and `private:` with the class declaration;
indent members by four spaces. Indent `case` labels once inside `switch` and their bodies once more.

```cpp
namespace GhidraEngine {

class FlatPdqIndex {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

private:
    std::vector<PdqIndexEntry> entries_;
};

}
```

Write `if (condition)`, `for (...)` and `switch (...)`; calls have no space before `(`.
Use spaces around binary operators and after commas. Bind pointers and references to names,
for example `const PdqHash &hash` and `AVFrame *frame`.

Separate functions and independent declarations with one blank line. Inside functions,
separate preparation, validation, processing and results. Separate loops from setup and
following results; inside loops, separate guards from the work they protect.
Put a blank line before an early `return` when other work precedes it. A lone return needs none.

### Line breaks

Keep short signatures, calls and aggregate initializers on one line. For long ones, put each
argument or element on its own line, indent by four spaces and put the closing delimiter on
its own line. Do not pack arguments to fill the width.

```cpp
std::vector<ImageMatch> verify_image_candidates(
    MediaId query_id,
    const ImageSignature &query,
    std::span<const FingerprintId> candidates,
    const ImageFingerprintCatalog &catalog,
    const PdqMatchPolicy &policy
) {
    // Implementation.
}
```

A long return type may occupy its own line. Start constructor initializer lists on the next
line with an indented `:`. Put template declarations on their own line. Break conditions at
logical boundaries and align continuation operands. Use a named intermediate value when
nesting makes an expression hard to read. Keep lambdas with their calls when readable;
format their bodies like other functions.

### Includes and headers

In a `.cpp`, include its corresponding public header first when one exists. Separate the
following groups with blank lines and sort each group by case-sensitive header name:

1. Corresponding public header.
2. Other project headers.
3. Private headers.
4. Third-party headers.
5. Standard-library headers.

Use `<GhidraEngine/...>` for public headers and quotes for private relative headers.
Include what the file directly uses. Keep required C-library headers in their `extern "C"` block.

Public headers use `GHIDRAENGINE_<PATH>_HPP` guards and `GHIDRAENGINE_EXPORT` for exported
symbols. Preserve existing private-header guards or `#pragma once`. Public declarations belong
in `GhidraEngine`, shared internals in `GhidraEngine::detail`, and file-local helpers in an
anonymous namespace. Follow the file's namespace-closing comment convention.

### Names and declarations

| Item                            | Convention                                 |
|---------------------------------|--------------------------------------------|
| Types and enum values           | `PascalCase`                               |
| Functions, variables and fields | `snake_case`                               |
| Private storage                 | Trailing underscore, such as `entries_`    |
| Public aggregate fields         | No trailing underscore                     |
| Macros                          | Uppercase with the existing project prefix |

Use `const` for unchanged values and `const auto &` for borrowed elements. Use `auto` when
its initializer makes the type clear; spell out types that communicate units, ownership or
numeric width. Use `{}` for zero/default initialization and aggregates, and `= value` for
ordinary scalar defaults. Follow nearby declarations for top-level parameter `const`.
Preserve meaningful `[[nodiscard]]`, `explicit`, `constexpr` and `noexcept`.

### Tests and benchmarks

Apply the same formatting rules as production code. Preserve framework names such as
`TEST(Core, CoverageRequiresFiniteUnitFraction)`; established benchmark names such as
`Search` and `Build` are naming exceptions.

In tests, separate setup, the operation and assertions. Keep related assertions together.
Format helpers normally; do not compress loops or error handling.

In benchmarks, separate setup, correctness checks, the timed loop and counters. Separate
helpers and registrations with blank lines. Wrap long registration chains at `->`, with one
operation per continuation line. Preserve exactly what runs inside the timed loop.

## CMake

### Commands and blocks

Use lowercase commands without a space before `(`, such as `if(WIN32)`. Use uppercase
keywords and scopes, including `PUBLIC`, `PRIVATE`, `REQUIRED`, `CONFIG`, `TARGETS`, `COMMAND`
and `VERBATIM`.

Indent block contents by four spaces and align closing commands with their openings.
Use `endif()`, `endforeach()` and `endfunction()` without repeating arguments.
Keep short commands on one line. Separate options, package discovery, targets, installation
and tests with blank lines.

### Multiline commands

Put each source, library, path, property pair or command argument on its own line. A keyword
may share a line with one short value, such as `VERSION "${PROJECT_VERSION}"`. Put the closing
`)` on its own line. In target commands, the target may stay on the opening line; indent
multiple entries beneath each scope by another four spaces.

```cmake
target_link_libraries(ghidra_tests
    PRIVATE
        GhidraEngine
        GTest::gtest_main
        Threads::Threads
)
```

For `install()` and `configure_package_config_file()`, the opening parenthesis may be on
the next line. Use one keyword group per line:

```cmake
install(
    FILES
        LICENSE
        third_party/licenses/PDQ-LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/GhidraEngine"
)
```

Separate independent test registrations and executable setups with blank lines. In long
`add_test()` and `add_custom_command()` calls, put command arguments on individual lines.

### Variables, paths and targets

- Keep established targets such as `GhidraEngine`, `GhidraEngine::GhidraEngine`,
  `ghidra_tests` and `ghidra_benchmarks`.
- Use uppercase `GHIDRAENGINE_...` for project options, `_snake_case` for temporary local
  variables and `snake_case` for function parameters. Keep standard CMake option names.
- Quote expanded paths and strings that must remain one argument. Preserve intentional list
  expansion and generator expressions; do not mechanically change quoting.
- Use forward slashes in paths, existing package targets and `target_*` with explicit scopes.
  Do not introduce global include or link paths.
- List maintained sources explicitly and group them by module. Do not replace lists with globs.

## Formatting checks

`.clang-format` provides an LLVM-based baseline with four-space indentation, a 100-column
limit, right-aligned pointers/references and case-sensitive include sorting. It does not
express every layout rule above and does not format CMake.

Review formatter output against this guide and neighboring maintained code. For CMake,
check keyword grouping, paths, quoting, scopes, list expansion, generator expressions,
parentheses and argument order. Formatting-only changes need no new helpers or dependencies.
