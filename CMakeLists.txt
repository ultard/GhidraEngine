cmake_minimum_required(VERSION 3.25)
project(GhidraEngine VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_TESTING "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(GHIDRAENGINE_BUILD_BENCHMARKS "Build benchmark harness" OFF)

set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(GNUInstallDirs)
include(GenerateExportHeader)
include(cmake/Dependencies.cmake)

add_library(GhidraEngine
    src/version.cpp
    src/cache/fingerprint.cpp
    src/cpu/kernels.cpp
    src/hash/blake3.cpp
    src/hash/pdq.cpp
    src/image/decode.cpp
    src/image/fingerprint.cpp
    src/index/flat_pdq.cpp
    src/index/mih_pdq.cpp
    src/index/video.cpp
    src/io/fingerprint.cpp
    src/match/image.cpp
    src/match/video.cpp
    src/scan/scanner.cpp
    src/video/decode.cpp
    src/video/fingerprint.cpp
)
add_library(GhidraEngine::GhidraEngine ALIAS GhidraEngine)

target_compile_features(GhidraEngine PUBLIC cxx_std_20)

target_compile_definitions(GhidraEngine
    PRIVATE
        GHIDRAENGINE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(GhidraEngine
    PRIVATE
        BLAKE3::blake3
        PDQ::pdq
        libvips::vips
        ffmpeg::avformat
        ffmpeg::avcodec
        ffmpeg::avutil
        ffmpeg::swscale
        Threads::Threads
)

generate_export_header(GhidraEngine
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/GhidraEngine/export.hpp"
    EXPORT_MACRO_NAME GHIDRAENGINE_EXPORT
    STATIC_DEFINE GHIDRAENGINE_STATIC_DEFINE
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(GhidraEngine PUBLIC GHIDRAENGINE_STATIC_DEFINE)
endif()

target_include_directories(GhidraEngine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

include(cmake/CpuKernels.cmake)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(GhidraEngine PRIVATE -Wimplicit-fallthrough)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(GhidraEngine PUBLIC -fexperimental-library)
endif()

include(cmake/Sanitizers.cmake)
include(cmake/Runtime.cmake)

if(BUILD_SHARED_LIBS)
    ghidraengine_copy_runtime(GhidraEngine)
endif()

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(GHIDRAENGINE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(cmake/Install.cmake)
