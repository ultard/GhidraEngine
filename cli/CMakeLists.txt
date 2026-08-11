find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(ghidraengine_cli main.cpp)

set_target_properties(ghidraengine_cli PROPERTIES OUTPUT_NAME ghidraengine)

target_link_libraries(ghidraengine_cli
    PRIVATE
        ghidraengine::ghidraengine
        ghidraengine_flags
        CLI11::CLI11
        nlohmann_json::nlohmann_json)

install(TARGETS ghidraengine_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
