# Shared Windows helpers belong here. Windows SDK types remain confined to
# this platform target and Windows backend-specific headers.

if(NOT WIN32)
    return()
endif()

add_library(qtav_platform_windows
    src/d3d11_device_access.cpp
)
add_library(QtAV::PlatformWindows ALIAS qtav_platform_windows)

target_compile_features(qtav_platform_windows PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_platform_windows
    PRIVATE QTAV_PLATFORM_WINDOWS_BUILDING_LIBRARY
)
target_include_directories(qtav_platform_windows
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_platform_windows PUBLIC d3d11)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_platform_windows
        PUBLIC QTAV_PLATFORM_WINDOWS_STATIC
    )
endif()

set_target_properties(qtav_platform_windows PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME PlatformWindows
)

if(MSVC)
    target_compile_options(qtav_platform_windows PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_platform_windows
        PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_platform_windows
        EXPORT QtAVCoreTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        DIRECTORY include/
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
endif()
