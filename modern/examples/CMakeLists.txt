add_executable(qtav_core_console console_player.cpp)
target_compile_features(qtav_core_console PRIVATE cxx_std_17)
target_link_libraries(qtav_core_console PRIVATE QtAV::Core)

if(TARGET qtav_audio_coreaudio AND TARGET qtav_audio_resample)
    target_link_libraries(
        qtav_core_console
        PRIVATE
            QtAV::AudioCoreAudio
            QtAV::AudioResample
    )
    target_compile_definitions(
        qtav_core_console
        PRIVATE QTAV_CORE_CONSOLE_HAS_COREAUDIO
    )
elseif(TARGET qtav_audio_wasapi AND TARGET qtav_audio_resample)
    target_link_libraries(
        qtav_core_console
        PRIVATE
            QtAV::AudioWASAPI
            QtAV::AudioResample
    )
    target_compile_definitions(
        qtav_core_console
        PRIVATE QTAV_CORE_CONSOLE_HAS_WASAPI
    )
endif()
