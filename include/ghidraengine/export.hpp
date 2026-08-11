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
