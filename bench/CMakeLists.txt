find_package(benchmark CONFIG REQUIRED)

add_executable(ghidraengine_bench
    ${CMAKE_SOURCE_DIR}/tests/image_fixture.cpp
    bench_main.cpp)

target_include_directories(ghidraengine_bench
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)

target_link_libraries(ghidraengine_bench
    PRIVATE
        ghidraengine::ghidraengine
        ghidraengine_flags
        benchmark::benchmark)

target_compile_definitions(ghidraengine_bench PRIVATE ${GHIDRAENGINE_SIMD_DEFINES})

if(TARGET libjpeg-turbo::jpeg)
    target_link_libraries(ghidraengine_bench PRIVATE libjpeg-turbo::jpeg)
elseif(TARGET libjpeg-turbo::turbojpeg)
    target_link_libraries(ghidraengine_bench PRIVATE libjpeg-turbo::turbojpeg)
else()
    target_link_libraries(ghidraengine_bench PRIVATE JPEG::JPEG)
endif()
