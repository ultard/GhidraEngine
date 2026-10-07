cmake_minimum_required(VERSION 3.25)
project(pdq LANGUAGES CXX)

add_library(pdq STATIC
    pdq/cpp/hashing/pdqhashing.cpp
    pdq/cpp/hashing/torben.cpp
    pdq/cpp/downscaling/downscaling.cpp)

target_compile_features(pdq PUBLIC cxx_std_11)
target_include_directories(pdq PUBLIC .)
set_target_properties(pdq PROPERTIES CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pdq PRIVATE -fno-fast-math -ffp-contract=off)
elseif(MSVC)
    target_compile_options(pdq PRIVATE /fp:strict)
endif()

include(GNUInstallDirs)
install(TARGETS pdq ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY pdq/ DESTINATION include/pdq FILES_MATCHING PATTERN "*.h")
