find_package(FFmpeg QUIET COMPONENTS avutil swscale)
if(NOT FFmpeg_avutil_FOUND OR NOT FFmpeg_swscale_FOUND)
    return()
endif()

add_library(qtav_render_cpu
    src/cpu_video_renderer.cpp
)
add_library(QtAV::RenderCPU ALIAS qtav_render_cpu)

target_compile_features(qtav_render_cpu PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_cpu
    PRIVATE QTAV_RENDER_CPU_BUILDING_LIBRARY
)
target_include_directories(qtav_render_cpu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_render_cpu
    PUBLIC QtAV::Core
    PRIVATE FFmpeg::avutil FFmpeg::swscale
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(qtav_render_cpu PUBLIC QTAV_RENDER_CPU_STATIC)
endif()

set_target_properties(qtav_render_cpu PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderCPU
)

if(MSVC)
    target_compile_options(qtav_render_cpu PRIVATE /W4 /permissive-)
else()
    target_compile_options(qtav_render_cpu PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_render_cpu
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
