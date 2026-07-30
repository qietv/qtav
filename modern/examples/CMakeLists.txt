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

if(TARGET qtav_interop_d3d11
   AND TARGET qtav_hw_d3d11va
   AND TARGET qtav_render_d3d11)
    target_link_libraries(
        qtav_core_console
        PRIVATE
            QtAV::InteropD3D11
            QtAV::HWD3D11VA
    )
    target_compile_definitions(
        qtav_core_console
        PRIVATE QTAV_CORE_CONSOLE_HAS_D3D11_VIDEO
    )
endif()
