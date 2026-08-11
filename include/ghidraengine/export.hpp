// GHIDRAENGINE_STATIC: set by the CMake target for a static archive.
// GHIDRAENGINE_BUILDING: set only while compiling the library itself.
#pragma once

#if defined(GHIDRAENGINE_STATIC)
#define GHIDRAENGINE_API
#elif defined(_WIN32)
#if defined(GHIDRAENGINE_BUILDING)
#define GHIDRAENGINE_API __declspec(dllexport)
#else
#define GHIDRAENGINE_API __declspec(dllimport)
#endif
#else
#if defined(GHIDRAENGINE_BUILDING)
#define GHIDRAENGINE_API __attribute__((visibility("default")))
#else
#define GHIDRAENGINE_API
#endif
#endif
