if(NOT APPLE
   OR CMAKE_SYSTEM_NAME MATCHES "iOS|tvOS|watchOS|visionOS")
    return()
endif()

find_library(QTAV_AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
find_library(QTAV_COREAUDIO_FRAMEWORK CoreAudio)
find_library(QTAV_COREFOUNDATION_FRAMEWORK CoreFoundation)
if(NOT QTAV_AUDIOTOOLBOX_FRAMEWORK
   OR NOT QTAV_COREAUDIO_FRAMEWORK
   OR NOT QTAV_COREFOUNDATION_FRAMEWORK)
    return()
endif()

add_library(qtav_audio_coreaudio
    src/coreaudio_audio_sink.cpp
)
add_library(QtAV::AudioCoreAudio ALIAS qtav_audio_coreaudio)

target_compile_features(qtav_audio_coreaudio PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_audio_coreaudio
    PRIVATE QTAV_AUDIO_COREAUDIO_BUILDING_LIBRARY
)
target_include_directories(qtav_audio_coreaudio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_audio_coreaudio
    PUBLIC
        QtAV::Core
        "${QTAV_AUDIOTOOLBOX_FRAMEWORK}"
        "${QTAV_COREAUDIO_FRAMEWORK}"
        "${QTAV_COREFOUNDATION_FRAMEWORK}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_audio_coreaudio
        PUBLIC QTAV_AUDIO_COREAUDIO_STATIC
    )
endif()

set_target_properties(qtav_audio_coreaudio PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME AudioCoreAudio
)

target_compile_options(
    qtav_audio_coreaudio
    PRIVATE -Wall -Wextra -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_audio_coreaudio
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
