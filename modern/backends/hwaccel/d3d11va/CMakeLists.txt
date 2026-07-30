if(NOT WIN32)
    return()
endif()

find_package(FFmpeg REQUIRED COMPONENTS avutil)

add_library(qtav_hw_d3d11va
    src/d3d11va_hardware_decoder.cpp
)
add_library(QtAV::HWD3D11VA ALIAS qtav_hw_d3d11va)

target_compile_features(qtav_hw_d3d11va PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_hw_d3d11va
    PRIVATE QTAV_HW_D3D11VA_BUILDING_LIBRARY
)
target_include_directories(qtav_hw_d3d11va
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/core/src"
        "${PROJECT_SOURCE_DIR}/platform/windows/src"
)
target_link_libraries(qtav_hw_d3d11va
    PUBLIC
        QtAV::Core
        QtAV::PlatformWindows
    PRIVATE
        FFmpeg::avutil
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_hw_d3d11va
        PUBLIC QTAV_HW_D3D11VA_STATIC
    )
endif()

set_target_properties(qtav_hw_d3d11va PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME HWD3D11VA
)

if(MSVC)
    target_compile_options(
        qtav_hw_d3d11va PRIVATE /W4 /permissive-
    )
else()
    target_compile_options(
        qtav_hw_d3d11va PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_hw_d3d11va
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
