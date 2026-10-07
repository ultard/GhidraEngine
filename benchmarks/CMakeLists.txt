find_package(benchmark CONFIG REQUIRED)

add_executable(ghidra_benchmarks
    cpu_bench.cpp
    pdq_index_bench.cpp
    vpdq_bench.cpp
    media_bench.cpp
    image_match_bench.cpp
    scan_bench.cpp
)

target_link_libraries(ghidra_benchmarks
    PRIVATE
        GhidraEngine
        benchmark::benchmark_main
)

target_compile_definitions(ghidra_benchmarks
    PRIVATE
        GHIDRAENGINE_TEST_DATA_PATH="${PROJECT_SOURCE_DIR}/tests/data"
)

ghidraengine_copy_runtime(ghidra_benchmarks)
