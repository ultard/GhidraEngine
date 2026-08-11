find_package(Catch2 3 CONFIG REQUIRED)

add_executable(ghidraengine_tests
    image_fixture.cpp
    test_units.cpp
    test_accuracy.cpp
    test_scan.cpp
    test_capi.cpp)

target_include_directories(ghidraengine_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ghidraengine_tests
    PRIVATE
        ghidraengine::ghidraengine
        ghidraengine_flags
        Catch2::Catch2WithMain)

# Tests reach into the library's internals (the DCT kernels, the MIH index, the
# format sniffer), so they need the same private definitions the library builds with.
target_compile_definitions(ghidraengine_tests PRIVATE ${GHIDRAENGINE_SIMD_DEFINES})

# The fixture generator encodes real JPEGs so the accuracy tests exercise the
# actual decode path rather than synthetic pixel buffers.
if(TARGET libjpeg-turbo::jpeg)
    target_link_libraries(ghidraengine_tests PRIVATE libjpeg-turbo::jpeg)
elseif(TARGET libjpeg-turbo::turbojpeg)
    target_link_libraries(ghidraengine_tests PRIVATE libjpeg-turbo::turbojpeg)
else()
    target_link_libraries(ghidraengine_tests PRIVATE JPEG::JPEG)
endif()

include(Catch)
catch_discover_tests(ghidraengine_tests)
