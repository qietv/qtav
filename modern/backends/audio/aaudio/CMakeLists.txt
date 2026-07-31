if(NOT ANDROID)
    return()
endif()

set(qtav_aaudio_android_api "${CMAKE_SYSTEM_VERSION}")
if(ANDROID_PLATFORM MATCHES "^android-([0-9]+)$")
    set(qtav_aaudio_android_api "${CMAKE_MATCH_1}")
endif()
if(qtav_aaudio_android_api
   AND qtav_aaudio_android_api VERSION_LESS 26)
    return()
endif()

add_library(qtav_audio_aaudio
    src/aaudio_audio_sink.cpp
)
add_library(QtAV::AudioAAudio ALIAS qtav_audio_aaudio)

target_compile_features(qtav_audio_aaudio PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_audio_aaudio
    PRIVATE QTAV_AUDIO_AAUDIO_BUILDING_LIBRARY
)
target_include_directories(qtav_audio_aaudio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(
    qtav_audio_aaudio
    PUBLIC QtAV::Core
    PRIVATE aaudio
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_audio_aaudio
        PUBLIC QTAV_AUDIO_AAUDIO_STATIC
    )
endif()

set_target_properties(qtav_audio_aaudio PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME AudioAAudio
)

target_compile_options(
    qtav_audio_aaudio
    PRIVATE -Wall -Wextra -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_audio_aaudio
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
