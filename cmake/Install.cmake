include(CMakePackageConfigHelpers)

configure_package_config_file(
    cmake/GhidraEngineConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/GhidraEngineConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/GhidraEngine"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/GhidraEngineConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion
)

install(
    TARGETS GhidraEngine
    EXPORT GhidraEngineTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

if(BUILD_SHARED_LIBS)
    if(WIN32)
        install(
            DIRECTORY "${CMAKE_BINARY_DIR}/runtime-deploy/"
            DESTINATION "${CMAKE_INSTALL_BINDIR}"
            OPTIONAL
            FILES_MATCHING PATTERN "*.dll"
        )
    else()
        if(APPLE)
            set_property(TARGET GhidraEngine PROPERTY INSTALL_RPATH "@loader_path")
        else()
            set_property(TARGET GhidraEngine PROPERTY INSTALL_RPATH "$ORIGIN")
        endif()

        install(
            DIRECTORY "${CMAKE_BINARY_DIR}/runtime-deploy/"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            OPTIONAL
            FILES_MATCHING
                PATTERN "*.so"
                PATTERN "*.so.*"
                PATTERN "*.dylib"
                PATTERN "*.dylib.*"
        )
    endif()
endif()

install(
    DIRECTORY include/GhidraEngine
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/include/GhidraEngine/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/GhidraEngine"
)

install(
    EXPORT GhidraEngineTargets
    NAMESPACE GhidraEngine::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/GhidraEngine"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/GhidraEngineConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/GhidraEngineConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/GhidraEngine"
)

install(
    FILES
        LICENSE
        third_party/licenses/FFMPEG-LICENSE
        third_party/licenses/PDQ-LICENSE
        third_party/licenses/VIPS-LICENSE
        third_party/licenses/VIPS-NSGIF-LICENSE
        third_party/licenses/RADIANCE-LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/GhidraEngine"
)
