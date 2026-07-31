add_library(qtav_render_mobile
    src/mobile_video_renderer.cpp
)
add_library(QtAV::RenderMobile ALIAS qtav_render_mobile)

target_compile_features(qtav_render_mobile PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_mobile
    PRIVATE QTAV_RENDER_MOBILE_BUILDING_LIBRARY
)
target_include_directories(qtav_render_mobile
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_render_mobile PUBLIC QtAV::Core)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_render_mobile PUBLIC QTAV_RENDER_MOBILE_STATIC)
endif()

set_target_properties(qtav_render_mobile PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderMobile
)

if(MSVC)
    target_compile_options(qtav_render_mobile PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_render_mobile
        PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_render_mobile
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
