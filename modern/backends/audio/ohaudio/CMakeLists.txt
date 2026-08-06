if(NOT CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$")
    return()
endif()

find_library(QTAV_OHAUDIO_LIBRARY ohaudio)
if(NOT QTAV_OHAUDIO_LIBRARY)
    return()
endif()

add_library(qtav_audio_ohaudio
    src/ohaudio_audio_sink.cpp
)
add_library(QtAV::AudioOHAudio ALIAS qtav_audio_ohaudio)

target_compile_features(qtav_audio_ohaudio PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_audio_ohaudio
    PRIVATE QTAV_AUDIO_OHAUDIO_BUILDING_LIBRARY
)
target_include_directories(qtav_audio_ohaudio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_audio_ohaudio
    PUBLIC QtAV::Core
    PRIVATE ohaudio
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_audio_ohaudio
        PUBLIC QTAV_AUDIO_OHAUDIO_STATIC
    )
endif()

set_target_properties(qtav_audio_ohaudio PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME AudioOHAudio
)

target_compile_options(
    qtav_audio_ohaudio
    PRIVATE -Wall -Wextra -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_audio_ohaudio
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
