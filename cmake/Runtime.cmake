function(ghidraengine_copy_runtime target)
    if(WIN32)
        get_filename_component(_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(_runtime_dirs "")

        foreach(_config Debug Release RelWithDebInfo MinSizeRel)
            string(TOUPPER "${_config}" _upper_config)

            foreach(_package libvips glib ffmpeg)
                foreach(_directory IN LISTS ${_package}_BIN_DIRS_${_upper_config})
                    list(APPEND _runtime_dirs "$<$<CONFIG:${_config}>:${_directory}>")
                endforeach()
            endforeach()
        endforeach()

        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DDLLS=$<TARGET_RUNTIME_DLLS:${target}>"
                "-DRUNTIME_DIRS=${_runtime_dirs}"
                "-DCOMPILER_BIN=${_compiler_bin}"
                "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
                -P
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CopyRuntimeDlls.cmake"
            VERBATIM
        )
    endif()
endfunction()
