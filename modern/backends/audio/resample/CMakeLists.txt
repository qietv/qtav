find_package(FFmpeg QUIET COMPONENTS avutil swresample)
if(NOT FFmpeg_avutil_FOUND OR NOT FFmpeg_swresample_FOUND)
    return()
endif()

add_library(qtav_audio_resample
    src/swresample_audio_converter.cpp
)
add_library(QtAV::AudioResample ALIAS qtav_audio_resample)

target_compile_features(qtav_audio_resample PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_audio_resample
    PRIVATE QTAV_AUDIO_RESAMPLE_BUILDING_LIBRARY
)
target_include_directories(qtav_audio_resample
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_audio_resample
    PUBLIC QtAV::Core
    PRIVATE FFmpeg::avutil FFmpeg::swresample
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_audio_resample
        PUBLIC QTAV_AUDIO_RESAMPLE_STATIC
    )
endif()

set_target_properties(qtav_audio_resample PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME AudioResample
)

if(MSVC)
    target_compile_options(qtav_audio_resample PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_audio_resample
        PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_audio_resample
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
