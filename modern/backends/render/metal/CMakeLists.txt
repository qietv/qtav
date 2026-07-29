if(NOT APPLE)
    return()
endif()

enable_language(OBJCXX)

find_library(QTAV_METAL_FRAMEWORK Metal)
find_library(QTAV_QUARTZCORE_FRAMEWORK QuartzCore)
find_library(QTAV_FOUNDATION_FRAMEWORK Foundation)
if(NOT QTAV_METAL_FRAMEWORK
   OR NOT QTAV_QUARTZCORE_FRAMEWORK
   OR NOT QTAV_FOUNDATION_FRAMEWORK)
    return()
endif()

add_library(qtav_render_metal
    src/metal_video_renderer.mm
)
add_library(QtAV::RenderMetal ALIAS qtav_render_metal)

target_compile_features(qtav_render_metal PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_metal
    PRIVATE QTAV_RENDER_METAL_BUILDING_LIBRARY
)
target_compile_options(qtav_render_metal PRIVATE -fobjc-arc)
target_include_directories(qtav_render_metal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_render_metal
    PUBLIC
        QtAV::Core
        "${QTAV_FOUNDATION_FRAMEWORK}"
        "${QTAV_METAL_FRAMEWORK}"
        "${QTAV_QUARTZCORE_FRAMEWORK}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(qtav_render_metal PUBLIC QTAV_RENDER_METAL_STATIC)
endif()

set_target_properties(qtav_render_metal PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderMetal
)

target_compile_options(
    qtav_render_metal
    PRIVATE -Wall -Wextra -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_render_metal
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
