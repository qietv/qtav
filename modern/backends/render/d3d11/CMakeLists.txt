if(NOT WIN32)
    return()
endif()

add_library(qtav_render_d3d11
    src/d3d11_video_renderer.cpp
)
add_library(QtAV::RenderD3D11 ALIAS qtav_render_d3d11)

target_compile_features(qtav_render_d3d11 PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_d3d11
    PRIVATE QTAV_RENDER_D3D11_BUILDING_LIBRARY
)
target_include_directories(qtav_render_d3d11
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_render_d3d11
    PUBLIC
        QtAV::Core
        QtAV::PlatformWindows
    PRIVATE
        d3dcompiler
        dxgi
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_render_d3d11
        PUBLIC QTAV_RENDER_D3D11_STATIC
    )
endif()

set_target_properties(qtav_render_d3d11 PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderD3D11
)

if(MSVC)
    target_compile_options(qtav_render_d3d11 PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_render_d3d11
        PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_render_d3d11
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
