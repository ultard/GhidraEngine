find_package(GTest CONFIG REQUIRED)

add_executable(ghidra_tests
    core/core_test.cpp
    core/cpu_test.cpp
    core/hash_test.cpp
    core/pdq_index_test.cpp
    core/serialization_test.cpp
    media/image_decode_test.cpp
    media/image_match_test.cpp
    media/video_test.cpp
    media/vpdq_test.cpp
    scan/scanner_cache_test.cpp
    scan/scan_progress_test.cpp
)

target_link_libraries(ghidra_tests
    PRIVATE
        GhidraEngine
        GTest::gtest_main
        Threads::Threads
        PDQ::pdq
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ghidra_tests PRIVATE -ffp-contract=off)
endif()

target_compile_definitions(ghidra_tests
    PRIVATE
        GHIDRAENGINE_TEST_DATA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

ghidraengine_copy_runtime(ghidra_tests)

include(GoogleTest)
gtest_discover_tests(ghidra_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 120
)

if(NOT BUILD_SHARED_LIBS AND GHIDRAENGINE_SANITIZER STREQUAL "none")
    add_executable(ghidra_allocation_test media/allocation_failure_test.cpp)
    target_link_libraries(ghidra_allocation_test PRIVATE GhidraEngine)
    ghidraengine_copy_runtime(ghidra_allocation_test)

    add_test(NAME VideoCatalog.AllocationFailure COMMAND ghidra_allocation_test)
    set_tests_properties(VideoCatalog.AllocationFailure PROPERTIES TIMEOUT 30)

    add_executable(ghidra_cache_allocation_test scan/cache_allocation_failure_test.cpp)
    target_link_libraries(ghidra_cache_allocation_test PRIVATE GhidraEngine)
    ghidraengine_copy_runtime(ghidra_cache_allocation_test)

    add_test(NAME FingerprintCache.AllocationFailure COMMAND ghidra_cache_allocation_test)
    set_tests_properties(FingerprintCache.AllocationFailure PROPERTIES TIMEOUT 30)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
        add_executable(ghidra_native_allocation_test media/native_allocation_failure_test.cpp)
        target_link_libraries(ghidra_native_allocation_test PRIVATE GhidraEngine)
        target_link_options(ghidra_native_allocation_test
            PRIVATE
                "-Wl,--wrap=avcodec_parameters_to_context"
        )
        ghidraengine_copy_runtime(ghidra_native_allocation_test)

        add_test(
            NAME VideoDecode.NativeAllocationFailure
            COMMAND ghidra_native_allocation_test
                "${CMAKE_CURRENT_SOURCE_DIR}/data/video/display-0.mp4"
        )
        set_tests_properties(VideoDecode.NativeAllocationFailure PROPERTIES TIMEOUT 30)
    endif()
endif()
