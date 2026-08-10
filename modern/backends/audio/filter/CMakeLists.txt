find_package(FFmpeg QUIET COMPONENTS avfilter avutil)
if(NOT FFmpeg_avfilter_FOUND OR NOT FFmpeg_avutil_FOUND)
    return()
endif()

add_library(qtav_audio_filter
    src/volume_audio_frame_processor.cpp
)
add_library(QtAV::AudioFilter ALIAS qtav_audio_filter)

target_compile_features(qtav_audio_filter PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_audio_filter
    PRIVATE QTAV_AUDIO_FILTER_BUILDING_LIBRARY
)
target_include_directories(qtav_audio_filter
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_audio_filter
    PUBLIC QtAV::Core
    PRIVATE FFmpeg::avfilter FFmpeg::avutil
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_audio_filter
        PUBLIC QTAV_AUDIO_FILTER_STATIC
    )
endif()

set_target_properties(qtav_audio_filter PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME AudioFilter
)

if(MSVC)
    target_compile_options(qtav_audio_filter PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_audio_filter
        PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_audio_filter
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
