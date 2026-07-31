if(NOT ANDROID)
    return()
endif()

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil)

add_library(qtav_hw_mediacodec
    src/mediacodec_hardware_decoder.cpp
)
add_library(QtAV::HWMediaCodec ALIAS qtav_hw_mediacodec)

target_compile_features(qtav_hw_mediacodec PUBLIC cxx_std_17)
target_include_directories(qtav_hw_mediacodec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/core/src"
)
target_link_libraries(qtav_hw_mediacodec
    PUBLIC QtAV::Core
    PRIVATE
        FFmpeg::avcodec
        FFmpeg::avutil
        android
        mediandk
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_hw_mediacodec
        PUBLIC QTAV_HW_MEDIACODEC_STATIC
    )
endif()

set_target_properties(qtav_hw_mediacodec PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME HWMediaCodec
)

target_compile_options(
    qtav_hw_mediacodec
    PRIVATE -Wall -Wextra -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_hw_mediacodec
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
