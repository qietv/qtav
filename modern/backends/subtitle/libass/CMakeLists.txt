find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(
        QTAV_LIBASS
        QUIET
        IMPORTED_TARGET
        GLOBAL
        "libass>=0.17.0"
    )
endif()

if(NOT TARGET PkgConfig::QTAV_LIBASS)
    return()
endif()

add_library(qtav_subtitle_libass
    src/libass_subtitle_renderer.cpp
)
add_library(QtAV::SubtitleLibass ALIAS qtav_subtitle_libass)

target_compile_features(qtav_subtitle_libass PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_subtitle_libass
    PRIVATE QTAV_SUBTITLE_LIBASS_BUILDING_LIBRARY
)
target_include_directories(qtav_subtitle_libass
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_subtitle_libass
    PUBLIC QtAV::Core
    PRIVATE PkgConfig::QTAV_LIBASS
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_subtitle_libass
        PUBLIC QTAV_SUBTITLE_LIBASS_STATIC
    )
endif()

set_target_properties(qtav_subtitle_libass PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME SubtitleLibass
)

if(MSVC)
    target_compile_options(
        qtav_subtitle_libass
        PRIVATE /W4 /permissive-
    )
else()
    target_compile_options(
        qtav_subtitle_libass
        PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_subtitle_libass
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
