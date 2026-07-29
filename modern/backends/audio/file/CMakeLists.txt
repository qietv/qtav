add_library(qtav_audio_file
    src/wav_audio_sink.cpp
)
add_library(QtAV::AudioFile ALIAS qtav_audio_file)

target_compile_features(qtav_audio_file PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_audio_file
    PRIVATE QTAV_AUDIO_FILE_BUILDING_LIBRARY
)
target_include_directories(qtav_audio_file
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_audio_file PUBLIC QtAV::Core)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(qtav_audio_file PUBLIC QTAV_AUDIO_FILE_STATIC)
endif()

set_target_properties(qtav_audio_file PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME AudioFile
)

if(MSVC)
    target_compile_options(qtav_audio_file PRIVATE /W4 /permissive-)
else()
    target_compile_options(qtav_audio_file PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_audio_file
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
