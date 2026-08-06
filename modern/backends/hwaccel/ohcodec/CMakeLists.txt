if(NOT CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$")
    return()
endif()

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil)
find_library(QTAV_OHCODEC_NATIVE_WINDOW_LIBRARY native_window)
if(NOT QTAV_OHCODEC_NATIVE_WINDOW_LIBRARY)
    return()
endif()

add_library(qtav_hw_ohcodec
    src/ohcodec_hardware_decoder.cpp
)
add_library(QtAV::HWOHCodec ALIAS qtav_hw_ohcodec)

target_compile_features(qtav_hw_ohcodec PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_hw_ohcodec
    PRIVATE QTAV_HW_OHCODEC_BUILDING_LIBRARY
)
target_include_directories(qtav_hw_ohcodec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/core/src"
)
target_link_libraries(qtav_hw_ohcodec
    PUBLIC QtAV::Core
    PRIVATE
        FFmpeg::avcodec
        FFmpeg::avutil
        "${QTAV_OHCODEC_NATIVE_WINDOW_LIBRARY}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_hw_ohcodec
        PUBLIC QTAV_HW_OHCODEC_STATIC
    )
endif()

set_target_properties(qtav_hw_ohcodec PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME HWOHCodec
)

target_compile_options(
    qtav_hw_ohcodec
    PRIVATE -Wall -Wextra -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_hw_ohcodec
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
