file(GLOB _compiler_runtime
    "${COMPILER_BIN}/libstdc++-6.dll"
    "${COMPILER_BIN}/libgcc_s_*.dll"
    "${COMPILER_BIN}/libwinpthread-1.dll"
)
list(APPEND DLLS ${_compiler_runtime})

foreach(_directory IN LISTS RUNTIME_DIRS)
    if(_directory)
        file(GLOB _package_runtime "${_directory}/*.dll")
        list(APPEND DLLS ${_package_runtime})
    endif()
endforeach()

if(DLLS)
    file(COPY ${DLLS} DESTINATION "${DESTINATION}")
endif()
