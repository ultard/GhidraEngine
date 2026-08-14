find_package(Catch2 3 CONFIG REQUIRED)

add_executable(ghidraengine_tests
    image_fixture.cpp
    test_units.cpp
    test_accuracy.cpp
    test_scan.cpp
    test_capi.cpp)

target_include_directories(ghidraengine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ghidraengine_tests
    PRIVATE
        ghidraengine_objects
        ghidraengine_flags
        Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(ghidraengine_tests)
