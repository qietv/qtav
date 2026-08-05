if(WIN32)
    return()
endif()

if(ANDROID)
    find_library(QTAV_VULKAN_LIBRARY_PATH vulkan)
    if(QTAV_VULKAN_LIBRARY_PATH)
        set(QTAV_VULKAN_LIBRARY vulkan)
    endif()
else()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(QTAV_VULKAN_LIBRARY Vulkan::Vulkan)
    endif()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(
        QTAV_LIBPLACEBO
        QUIET
        IMPORTED_TARGET
        GLOBAL
        "libplacebo>=7.351.0"
    )
endif()

find_package(FFmpeg REQUIRED COMPONENTS avutil)

if(NOT QTAV_VULKAN_LIBRARY
   OR NOT TARGET PkgConfig::QTAV_LIBPLACEBO
   OR NOT TARGET qtav_render_libplacebo_common)
    return()
endif()

set(QTAV_VULKAN_INTEROP_SHADER_SOURCES)
if(ANDROID)
    set(QTAV_GLSLC_HINTS)
    if(DEFINED ANDROID_NDK AND DEFINED ANDROID_HOST_TAG)
        list(APPEND QTAV_GLSLC_HINTS
            "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}")
    endif()
    find_program(
        QTAV_GLSLC_EXECUTABLE
        NAMES glslc
        HINTS ${QTAV_GLSLC_HINTS}
        NO_CMAKE_FIND_ROOT_PATH)
    if(NOT QTAV_GLSLC_EXECUTABLE)
        return()
    endif()
    set(QTAV_VULKAN_GENERATED_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${QTAV_VULKAN_GENERATED_DIRECTORY}")

    function(qtav_compile_interop_shader source stage symbol output_variable)
        set(spv "${QTAV_VULKAN_GENERATED_DIRECTORY}/${symbol}.spv")
        set(include_file
            "${QTAV_VULKAN_GENERATED_DIRECTORY}/${symbol}.inc")
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${QTAV_GLSLC_EXECUTABLE}"
                "-fshader-stage=${stage}"
                --target-env=vulkan1.0
                -O
                -o "${spv}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            VERBATIM)
        add_custom_command(
            OUTPUT "${include_file}"
            COMMAND "${CMAKE_COMMAND}"
                "-DINPUT=${spv}"
                "-DOUTPUT=${include_file}"
                "-DSYMBOL=${symbol}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
            DEPENDS "${spv}"
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
            VERBATIM)
        set("${output_variable}" "${include_file}" PARENT_SCOPE)
    endfunction()

    qtav_compile_interop_shader(
        shaders/external_normalize.vert
        vertex
        external_normalize_vert_spv
        QTAV_VULKAN_INTEROP_VERTEX_INCLUDE)
    qtav_compile_interop_shader(
        shaders/external_normalize.frag
        fragment
        external_normalize_frag_spv
        QTAV_VULKAN_INTEROP_FRAGMENT_INCLUDE)
    list(APPEND QTAV_VULKAN_INTEROP_SHADER_SOURCES
        "${QTAV_VULKAN_INTEROP_VERTEX_INCLUDE}"
        "${QTAV_VULKAN_INTEROP_FRAGMENT_INCLUDE}")
endif()

add_library(qtav_render_vulkan
    src/vulkan_video_renderer.cpp
    ${QTAV_VULKAN_INTEROP_SHADER_SOURCES}
)
add_library(QtAV::RenderVulkan ALIAS qtav_render_vulkan)

target_compile_features(qtav_render_vulkan PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_vulkan
    PRIVATE QTAV_RENDER_VULKAN_BUILDING_LIBRARY
)
target_include_directories(qtav_render_vulkan
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/core/src"
        $<$<BOOL:${ANDROID}>:${QTAV_VULKAN_GENERATED_DIRECTORY}>
)
target_link_libraries(qtav_render_vulkan
    PUBLIC
        QtAV::Core
        "${QTAV_VULKAN_LIBRARY}"
    PRIVATE
        PkgConfig::QTAV_LIBPLACEBO
        FFmpeg::avutil
        QtAV::RenderLibplaceboCommon
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_render_vulkan PUBLIC QTAV_RENDER_VULKAN_STATIC)
endif()

set_target_properties(qtav_render_vulkan PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderVulkan
)

if(MSVC)
    target_compile_options(qtav_render_vulkan PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_render_vulkan
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-missing-field-initializers
    )
endif()

if(ANDROID)
    add_subdirectory(android)
elseif(CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$")
    add_subdirectory(ohos)
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_render_vulkan
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
