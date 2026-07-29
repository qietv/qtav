if(NOT WIN32)
    return()
endif()

add_library(qtav_audio_wasapi
    src/wasapi_audio_sink.cpp
)
add_library(QtAV::AudioWASAPI ALIAS qtav_audio_wasapi)

target_compile_features(qtav_audio_wasapi PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_audio_wasapi
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        QTAV_AUDIO_WASAPI_BUILDING_LIBRARY
)
target_include_directories(qtav_audio_wasapi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(
    qtav_audio_wasapi
    PUBLIC QtAV::Core
    PRIVATE avrt ksuser ole32 uuid
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_audio_wasapi
        PUBLIC QTAV_AUDIO_WASAPI_STATIC
    )
endif()

set_target_properties(qtav_audio_wasapi PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME AudioWASAPI
)

if(MSVC)
    target_compile_options(
        qtav_audio_wasapi
        PRIVATE /W4 /permissive-
    )
else()
    target_compile_options(
        qtav_audio_wasapi
        PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_audio_wasapi
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
