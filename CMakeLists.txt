cmake_minimum_required(VERSION 3.25)

if(DEFINED ENV{WINDIR} AND CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{VCToolsInstallDir})
    message(FATAL_ERROR
        "No MSVC environment. Configure from a Developer PowerShell for VS 2022"
        "or use the windows-vs preset, which needs no environment.")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/ghidraengine/ghidraengine.hpp" _version_header)
string(REGEX MATCH "major = ([0-9]+)" _ "${_version_header}")
set(_version "${CMAKE_MATCH_1}")
string(REGEX MATCH "minor = ([0-9]+)" _ "${_version_header}")
string(APPEND _version ".${CMAKE_MATCH_1}")
string(REGEX MATCH "patch = ([0-9]+)" _ "${_version_header}")
string(APPEND _version ".${CMAKE_MATCH_1}")
unset(_version_header)

project(ghidraengine
    VERSION ${_version}
    DESCRIPTION "High-performance duplicate photo and video finder"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(BUILD_SHARED_LIBS "Build ghidraengine as a shared library" ON)
option(GHIDRAENGINE_BUILD_CLI "Build the ghidraengine command line tool" ON)
option(GHIDRAENGINE_BUILD_TESTS "Build unit tests" ON)
option(GHIDRAENGINE_BUILD_BENCHMARKS "Build micro benchmarks" OFF)
option(GHIDRAENGINE_ENABLE_LTO "Enable link time optimisation in optimised builds" ON)
option(GHIDRAENGINE_ENABLE_SIMD "Build SIMD kernels with runtime dispatch" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
else()
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
endif()

include(GNUInstallDirs)
include(CheckCXXSourceCompiles)

find_package(PkgConfig QUIET)
find_package(JPEG REQUIRED)

find_package(xxHash CONFIG QUIET)
if(NOT xxHash_FOUND)
    find_path(XXHASH_INCLUDE_DIR xxhash.h REQUIRED)
endif()

find_package(unofficial-sqlite3 CONFIG QUIET)
if(NOT unofficial-sqlite3_FOUND)
    find_package(SQLite3 REQUIRED)
endif()

find_package(FFMPEG QUIET)
if(NOT FFMPEG_FOUND)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
            libavcodec libavformat libavutil libswscale)
    else()
        message(FATAL_ERROR "FFmpeg not found: install it via vcpkg or provide pkg-config files")
    endif()
endif()

add_library(ghidraengine_flags INTERFACE)

if(MSVC)
    target_compile_options(ghidraengine_flags INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MP
        /D_CRT_SECURE_NO_WARNINGS
        $<$<NOT:$<CONFIG:Debug>>:/Oi /Gy /GS->)
else()
    # -fno-plt is an ELF thing; Apple clang just warns it is unused.
    target_compile_options(ghidraengine_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
        $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<PLATFORM_ID:Darwin>>>:-fno-plt>)
endif()

if(WIN32)
    target_compile_definitions(ghidraengine_flags INTERFACE
        _WIN32_WINNT=0x0602 WINVER=0x0602 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(GHIDRAENGINE_SIMD_SOURCES "")
set(GHIDRAENGINE_SIMD_DEFINES "")

if(GHIDRAENGINE_ENABLE_SIMD)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|amd64|i[3-6]86)")
        list(APPEND GHIDRAENGINE_SIMD_SOURCES src/hash/dct_avx2.cpp src/hash/dct_sse2.cpp)
        list(APPEND GHIDRAENGINE_SIMD_DEFINES GHIDRAENGINE_X86_SIMD=1)

        if(MSVC)
            set_source_files_properties(src/hash/dct_avx2.cpp
                PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        else()
            set_source_files_properties(src/hash/dct_avx2.cpp
                PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-mpopcnt")
            set_source_files_properties(src/hash/dct_sse2.cpp
                PROPERTIES COMPILE_OPTIONS "-msse2")
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64|ARM64)")
        list(APPEND GHIDRAENGINE_SIMD_SOURCES src/hash/dct_neon.cpp)
        list(APPEND GHIDRAENGINE_SIMD_DEFINES GHIDRAENGINE_NEON_SIMD=1)
    endif()
endif()

set(GHIDRAENGINE_SOURCES
    src/core/types.cpp
    src/core/config.cpp
    src/core/scanner.cpp
    src/core/enumerate.cpp
    src/core/thread_pool.cpp
    src/core/file_io.cpp
    src/core/glob.cpp
    src/hash/content_hash.cpp
    src/hash/dct.cpp
    src/hash/dct_scalar.cpp
    src/hash/phash.cpp
    src/decode/jpeg_turbo.cpp
    src/decode/ffmpeg_image.cpp
    src/decode/ffmpeg_common.cpp
    src/decode/resample.cpp
    src/video/video_signature.cpp
    src/video/video_match.cpp
    src/index/mih_index.cpp
    src/index/cluster.cpp
    src/cache/sqlite_cache.cpp
    src/capi/ghidraengine_c.cpp
)

if(WIN32)
    list(APPEND GHIDRAENGINE_SOURCES src/core/platform_win.cpp)
else()
    list(APPEND GHIDRAENGINE_SOURCES src/core/platform_posix.cpp)
endif()

add_library(ghidraengine_deps INTERFACE)

find_package(Threads REQUIRED)
target_link_libraries(ghidraengine_deps INTERFACE JPEG::JPEG Threads::Threads)

if(TARGET xxHash::xxhash)
    target_link_libraries(ghidraengine_deps INTERFACE xxHash::xxhash)
else()
    target_include_directories(ghidraengine_deps INTERFACE $<BUILD_INTERFACE:${XXHASH_INCLUDE_DIR}>)
    target_compile_definitions(ghidraengine_deps INTERFACE XXH_INLINE_ALL)
endif()

if(TARGET unofficial::sqlite3::sqlite3)
    target_link_libraries(ghidraengine_deps INTERFACE unofficial::sqlite3::sqlite3)
else()
    target_link_libraries(ghidraengine_deps INTERFACE SQLite::SQLite3)
endif()

if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(ghidraengine_deps INTERFACE PkgConfig::FFMPEG)
else()
    target_include_directories(ghidraengine_deps INTERFACE $<BUILD_INTERFACE:${FFMPEG_INCLUDE_DIRS}>)
    target_link_libraries(ghidraengine_deps INTERFACE ${FFMPEG_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(ghidraengine_deps INTERFACE shlwapi)
endif()

add_library(ghidraengine_objects OBJECT ${GHIDRAENGINE_SOURCES} ${GHIDRAENGINE_SIMD_SOURCES})

target_include_directories(ghidraengine_objects
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(ghidraengine_objects
    PUBLIC GHIDRAENGINE_BUILDING ${GHIDRAENGINE_SIMD_DEFINES})

target_link_libraries(ghidraengine_objects PUBLIC ghidraengine_deps PRIVATE ghidraengine_flags)

add_library(ghidraengine $<TARGET_OBJECTS:ghidraengine_objects>)
add_library(ghidraengine::ghidraengine ALIAS ghidraengine)

target_include_directories(ghidraengine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ghidraengine
    PRIVATE ghidraengine_flags ghidraengine_deps
    PUBLIC Threads::Threads)

get_target_property(GHIDRAENGINE_TYPE ghidraengine TYPE)
if(GHIDRAENGINE_TYPE STREQUAL "STATIC_LIBRARY")
    target_compile_definitions(ghidraengine PUBLIC GHIDRAENGINE_STATIC)
    target_compile_definitions(ghidraengine_objects PUBLIC GHIDRAENGINE_STATIC)
endif()

set_target_properties(ghidraengine PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME ghidraengine)

if(GHIDRAENGINE_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT GHIDRAENGINE_IPO_OK OUTPUT GHIDRAENGINE_IPO_MSG)
    if(GHIDRAENGINE_IPO_OK)
        # Both targets: the objects carry the compile side, ghidraengine the link side.
        set_target_properties(ghidraengine ghidraengine_objects PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    else()
        message(STATUS "LTO unavailable: ${GHIDRAENGINE_IPO_MSG}")
    endif()
endif()

if(GHIDRAENGINE_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(GHIDRAENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(GHIDRAENGINE_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

include(CMakePackageConfigHelpers)

install(TARGETS ghidraengine ghidraengine_flags ghidraengine_deps
    EXPORT ghidraengineTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/ghidraengine DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ghidraengineTargets
    FILE ghidraengineTargets.cmake
    NAMESPACE ghidraengine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ghidraengine)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ghidraengineConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ghidraengineConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ghidraengineConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ghidraengine)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ghidraengineConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ghidraengineConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ghidraengine)

message(STATUS "ghidraengine ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  library    : ${GHIDRAENGINE_TYPE}")
message(STATUS "  SIMD       : ${GHIDRAENGINE_ENABLE_SIMD}")
message(STATUS "  CLI        : ${GHIDRAENGINE_BUILD_CLI}")
message(STATUS "  tests      : ${GHIDRAENGINE_BUILD_TESTS}")
message(STATUS "  benchmarks : ${GHIDRAENGINE_BUILD_BENCHMARKS}")
