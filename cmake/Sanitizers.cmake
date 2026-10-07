set(GHIDRAENGINE_SANITIZER "none" CACHE STRING "none, address-undefined, or thread")
set_property(CACHE GHIDRAENGINE_SANITIZER PROPERTY STRINGS none address-undefined thread)

if(NOT GHIDRAENGINE_SANITIZER STREQUAL "none")
    if(WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers require GCC or Clang on a non-Windows platform")
    endif()

    if(GHIDRAENGINE_SANITIZER STREQUAL "address-undefined")
        set(_sanitizer_flag -fsanitize=address,undefined)
    elseif(GHIDRAENGINE_SANITIZER STREQUAL "thread")
        set(_sanitizer_flag -fsanitize=thread)
    else()
        message(FATAL_ERROR "Unknown GHIDRAENGINE_SANITIZER: ${GHIDRAENGINE_SANITIZER}")
    endif()

    target_compile_options(GhidraEngine
        PUBLIC
            ${_sanitizer_flag}
            -fno-omit-frame-pointer
    )
    target_link_options(GhidraEngine PUBLIC ${_sanitizer_flag})
endif()
