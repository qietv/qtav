find_package(FFmpeg QUIET COMPONENTS avfilter avutil)
if(NOT FFmpeg_avfilter_FOUND OR NOT FFmpeg_avutil_FOUND)
    return()
endif()

add_library(qtav_audio_timestretch
    src/atempo_audio_time_stretcher.cpp
)
add_library(QtAV::AudioTimeStretch ALIAS qtav_audio_timestretch)

target_compile_features(qtav_audio_timestretch PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_audio_timestretch
    PRIVATE QTAV_AUDIO_TIMESTRETCH_BUILDING_LIBRARY
)
target_include_directories(qtav_audio_timestretch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_audio_timestretch
    PUBLIC QtAV::Core
    PRIVATE FFmpeg::avfilter FFmpeg::avutil
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_audio_timestretch
        PUBLIC QTAV_AUDIO_TIMESTRETCH_STATIC
    )
endif()

set_target_properties(qtav_audio_timestretch PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME AudioTimeStretch
)

if(MSVC)
    target_compile_options(
        qtav_audio_timestretch PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_audio_timestretch
        PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_audio_timestretch
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
