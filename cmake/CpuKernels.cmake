option(GHIDRAENGINE_ENABLE_SIMD "Enable runtime-selected CPU kernels" ON)

if(GHIDRAENGINE_ENABLE_SIMD AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cpu_arch)

    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" _cpu_arch)
    endif()

    if(_cpu_arch MATCHES "^(x86_64|amd64)$")
        target_sources(GhidraEngine PRIVATE src/cpu/x86.cpp)
        target_compile_definitions(GhidraEngine PRIVATE GHIDRAENGINE_SIMD_X86)
    elseif(_cpu_arch MATCHES "^(aarch64|arm64)$")
        target_sources(GhidraEngine PRIVATE src/cpu/neon.cpp)
        target_compile_definitions(GhidraEngine PRIVATE GHIDRAENGINE_SIMD_NEON)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        src/cpu/kernels.cpp
        src/cpu/x86.cpp
        src/cpu/neon.cpp
        PROPERTIES COMPILE_OPTIONS -ffp-contract=off
    )
endif()
