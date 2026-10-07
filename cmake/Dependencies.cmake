if(CMAKE_PROJECT_TOP_LEVEL_INCLUDES MATCHES "conan_provider\\.cmake")
    list(APPEND CONAN_INSTALL_ARGS
        --deployer=runtime_deploy
        "--deployer-folder=${CMAKE_BINARY_DIR}/runtime-deploy"
        -s:h=compiler.cppstd=20
        -s:b=compiler.cppstd=20
        "-c:h=user.ghidraengine:tests=${BUILD_TESTING}"
        "-c:h=user.ghidraengine:benchmarks=${GHIDRAENGINE_BUILD_BENCHMARKS}"
    )

    if(CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_MAKE_PROGRAM)
        get_filename_component(_ninja_directory "${CMAKE_MAKE_PROGRAM}" DIRECTORY)

        if(CMAKE_HOST_WIN32)
            set(ENV{PATH} "${_ninja_directory};$ENV{PATH}")
        else()
            set(ENV{PATH} "${_ninja_directory}:$ENV{PATH}")
        endif()
    endif()
endif()

find_package(blake3 CONFIG REQUIRED)
find_package(pdq CONFIG REQUIRED)
find_package(libvips CONFIG REQUIRED)
find_package(ffmpeg CONFIG REQUIRED)
find_package(Threads REQUIRED)
