add_executable(qtav_core_api_test api_test.cpp)
target_link_libraries(qtav_core_api_test PRIVATE QtAV::Core)
target_compile_features(qtav_core_api_test PRIVATE cxx_std_17)
add_test(NAME qtav_core_api COMMAND qtav_core_api_test)

if(QTAV_CORE_INSTALL AND NOT CMAKE_CROSSCOMPILING)
    include(GNUInstallDirs)

    foreach(package_version_mode IN ITEMS
            EXACT
            COMPATIBLE
            REJECT_NEWER
            REJECT_OTHER_MAJOR)
        string(
            TOLOWER
            "${package_version_mode}"
            package_version_mode_lower
        )
        set(package_version_probe_command
            "${CMAKE_COMMAND}"
            -S "${CMAKE_CURRENT_SOURCE_DIR}/package_version"
            -B
                "${CMAKE_CURRENT_BINARY_DIR}/package-version-${package_version_mode_lower}"
            -G "${CMAKE_GENERATOR}"
            "-DQTAV_CORE_VERSION_FILE=${PROJECT_BINARY_DIR}/QtAVCoreConfigVersion.cmake"
            "-DQTAV_CORE_PROBE_MODE=${package_version_mode}"
        )
        if(CMAKE_GENERATOR_PLATFORM)
            list(APPEND package_version_probe_command
                -A "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        if(CMAKE_GENERATOR_TOOLSET)
            list(APPEND package_version_probe_command
                -T "${CMAKE_GENERATOR_TOOLSET}")
        endif()
        add_test(
            NAME
                "qtav_core_package_version_${package_version_mode_lower}"
            COMMAND ${package_version_probe_command}
        )
        set_tests_properties(
            "qtav_core_package_version_${package_version_mode_lower}"
            PROPERTIES TIMEOUT 30
        )
    endforeach()

    set(
        QTAV_CORE_TEST_INSTALL_PREFIX
        "${CMAKE_CURRENT_BINARY_DIR}/package-install/$<CONFIG>"
    )
    add_test(
        NAME qtav_core_package_install
        COMMAND
            "${CMAKE_COMMAND}"
            --install "${PROJECT_BINARY_DIR}"
            --config "$<CONFIG>"
            --prefix "${QTAV_CORE_TEST_INSTALL_PREFIX}"
    )
    set_tests_properties(
        qtav_core_package_install
        PROPERTIES
            FIXTURES_SETUP qtav_core_installed_package
            TIMEOUT 60
    )

    add_test(
        NAME qtav_core_installed_package_consumer
        COMMAND
            "${CMAKE_COMMAND}"
            "-DQTAV_CORE_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/install_consumer"
            "-DQTAV_CORE_CONSUMER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-consumer/$<CONFIG>"
            "-DQTAV_CORE_INSTALL_PREFIX=${QTAV_CORE_TEST_INSTALL_PREFIX}"
            "-DQTAV_CORE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DQTAV_CORE_CONSUMER_CONFIG=$<CONFIG>"
            "-DQTAV_CORE_CONSUMER_GENERATOR=${CMAKE_GENERATOR}"
            "-DQTAV_CORE_CONSUMER_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DQTAV_CORE_CONSUMER_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DQTAV_CORE_CONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
            "-DQTAV_CORE_CONSUMER_VCPKG_TRIPLET=${VCPKG_TARGET_TRIPLET}"
            "-DQTAV_CORE_CONSUMER_VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}"
            "-DQTAV_CORE_CONSUMER_PKG_CONFIG=${PKG_CONFIG_EXECUTABLE}"
            -P
                "${CMAKE_CURRENT_SOURCE_DIR}/run_install_consumer.cmake"
    )
    set_tests_properties(
        qtav_core_installed_package_consumer
        PROPERTIES
            FIXTURES_REQUIRED qtav_core_installed_package
            TIMEOUT 120
    )
endif()

find_package(FFmpeg REQUIRED COMPONENTS avutil)
add_executable(
    qtav_core_hardware_decode_device_test
    hardware_decode_device_test.cpp
)
target_link_libraries(
    qtav_core_hardware_decode_device_test
    PRIVATE
        QtAV::Core
        FFmpeg::avutil
)
target_include_directories(
    qtav_core_hardware_decode_device_test
    PRIVATE "${PROJECT_SOURCE_DIR}/core/src"
)
target_compile_features(
    qtav_core_hardware_decode_device_test
    PRIVATE cxx_std_17
)
add_test(
    NAME qtav_core_hardware_decode_device
    COMMAND qtav_core_hardware_decode_device_test
)

add_executable(qtav_core_backend_api_test backend_api_test.cpp)
target_link_libraries(qtav_core_backend_api_test PRIVATE QtAV::Core)
target_compile_features(qtav_core_backend_api_test PRIVATE cxx_std_17)
add_test(NAME qtav_core_backend_api COMMAND qtav_core_backend_api_test)

add_executable(
    qtav_mediacodec_image_epoch_test
    mediacodec_image_epoch_test.cpp
)
target_include_directories(
    qtav_mediacodec_image_epoch_test
    PRIVATE "${PROJECT_SOURCE_DIR}/backends/interop"
)
target_compile_features(
    qtav_mediacodec_image_epoch_test
    PRIVATE cxx_std_17
)
add_test(
    NAME qtav_mediacodec_image_epoch
    COMMAND qtav_mediacodec_image_epoch_test
)

add_executable(
    qtav_vulkan_raw_ycbcr_contract_test
    vulkan_raw_ycbcr_contract_test.cpp
)
target_include_directories(
    qtav_vulkan_raw_ycbcr_contract_test
    PRIVATE "${PROJECT_SOURCE_DIR}/backends/interop"
)
target_compile_definitions(
    qtav_vulkan_raw_ycbcr_contract_test
    PRIVATE
        "QTAV_MEDIACODEC_VULKAN_SOURCE=\"${PROJECT_SOURCE_DIR}/backends/interop/mediacodec_vulkan/src/mediacodec_vulkan_interop.cpp\""
        "QTAV_OHCODEC_VULKAN_SOURCE=\"${PROJECT_SOURCE_DIR}/backends/interop/ohcodec_vulkan/src/ohcodec_vulkan_interop.cpp\""
        "QTAV_VULKAN_EXTERNAL_NORMALIZER_SHADER=\"${PROJECT_SOURCE_DIR}/backends/render/vulkan/shaders/external_normalize.frag\""
)
target_compile_features(
    qtav_vulkan_raw_ycbcr_contract_test
    PRIVATE cxx_std_17
)
add_test(
    NAME qtav_vulkan_raw_ycbcr_contract
    COMMAND qtav_vulkan_raw_ycbcr_contract_test
)

if(NOT QTAV_AUDIO_AAUDIO STREQUAL "OFF")
    add_executable(
        qtav_audio_aaudio_pcm_queue_test
        aaudio_pcm_queue_test.cpp
    )
    target_compile_features(
        qtav_audio_aaudio_pcm_queue_test
        PRIVATE cxx_std_17
    )
    add_test(
        NAME qtav_audio_aaudio_pcm_queue
        COMMAND qtav_audio_aaudio_pcm_queue_test
    )
endif()

if(TARGET qtav_render_mobile)
    add_executable(
        qtav_render_mobile_test
        mobile_video_renderer_test.cpp
    )
    target_link_libraries(
        qtav_render_mobile_test
        PRIVATE QtAV::RenderMobile
    )
    target_include_directories(
        qtav_render_mobile_test
        PRIVATE "${PROJECT_SOURCE_DIR}/core/src"
    )
    target_compile_features(
        qtav_render_mobile_test
        PRIVATE cxx_std_17
    )
    add_test(
        NAME qtav_render_mobile_selector
        COMMAND qtav_render_mobile_test
    )
endif()

add_executable(
    qtav_core_frame_color_metadata_test
    frame_color_metadata_test.cpp
    "${PROJECT_SOURCE_DIR}/core/src/backend.cpp"
    "${PROJECT_SOURCE_DIR}/core/src/frame.cpp"
)
target_link_libraries(
    qtav_core_frame_color_metadata_test
    PRIVATE
        FFmpeg::avutil
)
target_include_directories(
    qtav_core_frame_color_metadata_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/core/include"
        "${PROJECT_SOURCE_DIR}/core/src"
)
target_compile_definitions(
    qtav_core_frame_color_metadata_test
    PRIVATE QTAV_CORE_STATIC
)
target_compile_features(
    qtav_core_frame_color_metadata_test
    PRIVATE cxx_std_17
)
add_test(
    NAME qtav_core_frame_color_metadata
    COMMAND qtav_core_frame_color_metadata_test
)

add_library(
    qtav_test_simulated_audio_sink
    STATIC
    simulated_audio_sink.cpp
)
target_link_libraries(qtav_test_simulated_audio_sink PUBLIC QtAV::Core)
target_compile_features(
    qtav_test_simulated_audio_sink
    PUBLIC cxx_std_17
)
target_include_directories(
    qtav_test_simulated_audio_sink
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(
    qtav_core_simulated_audio_sink_test
    simulated_audio_sink_test.cpp
)
target_link_libraries(
    qtav_core_simulated_audio_sink_test
    PRIVATE qtav_test_simulated_audio_sink
)
target_compile_features(
    qtav_core_simulated_audio_sink_test
    PRIVATE cxx_std_17
)
add_test(
    NAME qtav_core_simulated_audio_sink
    COMMAND qtav_core_simulated_audio_sink_test
)

add_executable(qtav_core_playback_test playback_test.cpp)
target_link_libraries(qtav_core_playback_test PRIVATE QtAV::Core)
target_compile_features(qtav_core_playback_test PRIVATE cxx_std_17)

add_executable(qtav_core_packet_buffer_test packet_buffer_test.cpp)
target_link_libraries(qtav_core_packet_buffer_test PRIVATE QtAV::Core)
target_compile_features(qtav_core_packet_buffer_test PRIVATE cxx_std_17)

add_executable(
    qtav_core_live_playback_policy_test
    live_playback_policy_test.cpp
)
target_link_libraries(
    qtav_core_live_playback_policy_test
    PRIVATE QtAV::Core
)
target_compile_features(
    qtav_core_live_playback_policy_test
    PRIVATE cxx_std_17
)

add_executable(qtav_core_control_test control_test.cpp)
target_link_libraries(qtav_core_control_test PRIVATE QtAV::Core)
target_compile_features(qtav_core_control_test PRIVATE cxx_std_17)

add_executable(qtav_core_frame_step_seek_test frame_step_seek_test.cpp)
target_link_libraries(qtav_core_frame_step_seek_test PRIVATE QtAV::Core)
target_compile_features(
    qtav_core_frame_step_seek_test
    PRIVATE cxx_std_17
)

add_executable(
    qtav_core_network_recovery_test
    network_recovery_test.cpp
)
target_link_libraries(
    qtav_core_network_recovery_test
    PRIVATE QtAV::Core
)
target_compile_features(
    qtav_core_network_recovery_test
    PRIVATE cxx_std_17
)

add_executable(qtav_core_track_switch_test track_switch_test.cpp)
target_link_libraries(
    qtav_core_track_switch_test
    PRIVATE
        QtAV::Core
        qtav_test_simulated_audio_sink
)
target_compile_features(qtav_core_track_switch_test PRIVATE cxx_std_17)

add_executable(qtav_core_external_track_test external_track_test.cpp)
target_link_libraries(qtav_core_external_track_test PRIVATE QtAV::Core)
target_compile_features(qtav_core_external_track_test PRIVATE cxx_std_17)

if(TARGET qtav_subtitle_libass)
    add_executable(
        qtav_subtitle_libass_test
        libass_subtitle_renderer_test.cpp
    )
    target_link_libraries(
        qtav_subtitle_libass_test
        PRIVATE QtAV::SubtitleLibass
    )
    target_compile_features(
        qtav_subtitle_libass_test
        PRIVATE cxx_std_17
    )
endif()

add_executable(qtav_core_audio_sink_player_test audio_sink_player_test.cpp)
target_link_libraries(qtav_core_audio_sink_player_test PRIVATE QtAV::Core)
target_compile_features(qtav_core_audio_sink_player_test PRIVATE cxx_std_17)

add_executable(
    qtav_core_processing_player_test
    processing_player_test.cpp
)
target_link_libraries(
    qtav_core_processing_player_test
    PRIVATE QtAV::Core
)
target_compile_features(
    qtav_core_processing_player_test
    PRIVATE cxx_std_17
)

add_executable(
    qtav_core_audio_time_stretch_player_test
    audio_time_stretch_player_test.cpp
)
target_link_libraries(
    qtav_core_audio_time_stretch_player_test
    PRIVATE QtAV::Core
)
target_compile_features(
    qtav_core_audio_time_stretch_player_test
    PRIVATE cxx_std_17
)

if(TARGET qtav_audio_timestretch)
    add_executable(
        qtav_audio_timestretch_test
        audio_time_stretcher_test.cpp
    )
    target_link_libraries(
        qtav_audio_timestretch_test
        PRIVATE QtAV::AudioTimeStretch
    )
    target_compile_features(
        qtav_audio_timestretch_test
        PRIVATE cxx_std_17
    )
    add_test(
        NAME qtav_audio_timestretch
        COMMAND qtav_audio_timestretch_test
    )
endif()

if(TARGET qtav_audio_filter)
    add_executable(
        qtav_audio_filter_test
        volume_audio_frame_processor_test.cpp
    )
    target_link_libraries(
        qtav_audio_filter_test
        PRIVATE QtAV::AudioFilter
    )
    target_compile_features(
        qtav_audio_filter_test
        PRIVATE cxx_std_17
    )
    add_test(
        NAME qtav_audio_filter
        COMMAND qtav_audio_filter_test
    )
endif()

if(TARGET qtav_audio_file)
    add_executable(qtav_audio_file_test wav_audio_sink_test.cpp)
    target_link_libraries(qtav_audio_file_test PRIVATE QtAV::AudioFile)
    target_compile_features(qtav_audio_file_test PRIVATE cxx_std_17)
    add_test(
        NAME qtav_audio_file
        COMMAND
            qtav_audio_file_test
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-audio-file-test.wav"
    )
endif()

if(TARGET qtav_audio_wasapi)
    add_executable(
        qtav_audio_wasapi_test
        wasapi_audio_sink_test.cpp
    )
    target_link_libraries(
        qtav_audio_wasapi_test
        PRIVATE QtAV::AudioWASAPI
    )
    target_compile_features(
        qtav_audio_wasapi_test
        PRIVATE cxx_std_17
    )
    add_test(
        NAME qtav_audio_wasapi
        COMMAND qtav_audio_wasapi_test
    )
    set_tests_properties(
        qtav_audio_wasapi
        PROPERTIES
            TIMEOUT 15
            SKIP_RETURN_CODE 77
    )
endif()

if(TARGET qtav_render_cpu)
    add_executable(qtav_render_cpu_test cpu_video_renderer_test.cpp)
    target_link_libraries(
        qtav_render_cpu_test
        PRIVATE QtAV::RenderCPU
    )
    target_compile_features(qtav_render_cpu_test PRIVATE cxx_std_17)
endif()

if(TARGET qtav_render_d3d11)
    add_executable(
        qtav_render_d3d11_test
        d3d11_video_renderer_test.cpp
    )
    target_link_libraries(
        qtav_render_d3d11_test
        PRIVATE
            QtAV::RenderD3D11
            FFmpeg::avutil
    )
    target_compile_features(
        qtav_render_d3d11_test
        PRIVATE cxx_std_17
    )
    target_include_directories(
        qtav_render_d3d11_test
        PRIVATE
            "${PROJECT_SOURCE_DIR}/backends/render/d3d11/src"
            "${PROJECT_SOURCE_DIR}/core/src"
    )

    add_executable(
        qtav_render_d3d11_advanced_color_test
        d3d11_advanced_color_test.cpp
    )
    target_link_libraries(
        qtav_render_d3d11_advanced_color_test
        PRIVATE
            QtAV::RenderD3D11
            FFmpeg::avutil
            dxgi
            user32
    )
    target_compile_features(
        qtav_render_d3d11_advanced_color_test
        PRIVATE cxx_std_17
    )
    target_include_directories(
        qtav_render_d3d11_advanced_color_test
        PRIVATE "${PROJECT_SOURCE_DIR}/core/src"
    )
    add_test(
        NAME qtav_render_d3d11_advanced_color
        COMMAND qtav_render_d3d11_advanced_color_test
    )
    set_tests_properties(
        qtav_render_d3d11_advanced_color
        PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 15
    )
endif()

if(TARGET qtav_output_d3d11)
    add_executable(
        qtav_output_d3d11_test
        d3d11_video_output_test.cpp
    )
    target_link_libraries(
        qtav_output_d3d11_test
        PRIVATE QtAV::OutputD3D11
    )
    target_compile_features(
        qtav_output_d3d11_test
        PRIVATE cxx_std_17
    )
endif()

if(TARGET qtav_render_vulkan)
    add_executable(
        qtav_render_vulkan_test
        vulkan_video_renderer_test.cpp
        vulkan_video_renderer_test_support.cpp
    )
    target_link_libraries(
        qtav_render_vulkan_test
        PRIVATE
            QtAV::RenderVulkan
            FFmpeg::avutil
    )
    target_include_directories(
        qtav_render_vulkan_test
        PRIVATE "${PROJECT_SOURCE_DIR}/core/src"
    )
    target_compile_features(
        qtav_render_vulkan_test
        PRIVATE cxx_std_17
    )
endif()

if(TARGET qtav_platform_windows)
    add_executable(
        qtav_platform_windows_test
        d3d11_device_access_test.cpp
    )
    target_link_libraries(
        qtav_platform_windows_test
        PRIVATE QtAV::PlatformWindows
    )
    target_compile_features(
        qtav_platform_windows_test
        PRIVATE cxx_std_17
    )
    add_test(
        NAME qtav_platform_windows_device_access
        COMMAND qtav_platform_windows_test
    )
    set_tests_properties(
        qtav_platform_windows_device_access
        PROPERTIES TIMEOUT 10
    )
endif()

if(TARGET qtav_interop_d3d11)
    add_executable(
        qtav_interop_d3d11_test
        d3d11_frame_interop_test.cpp
    )
    target_link_libraries(
        qtav_interop_d3d11_test
        PRIVATE
            QtAV::InteropD3D11
            QtAV::HWD3D11VA
            dxguid
    )
    target_compile_features(
        qtav_interop_d3d11_test
        PRIVATE cxx_std_17
    )
    add_test(
        NAME qtav_interop_d3d11_contract
        COMMAND qtav_interop_d3d11_test
    )
    set_tests_properties(
        qtav_interop_d3d11_contract
        PROPERTIES TIMEOUT 10
    )
endif()

if(TARGET qtav_hw_d3d11va)
    add_executable(
        qtav_hw_d3d11va_test
        d3d11va_hardware_decoder_test.cpp
    )
    target_link_libraries(
        qtav_hw_d3d11va_test
        PRIVATE
            QtAV::HWD3D11VA
            FFmpeg::avutil
    )
    target_include_directories(
        qtav_hw_d3d11va_test
        PRIVATE "${PROJECT_SOURCE_DIR}/core/src"
    )
    target_compile_features(
        qtav_hw_d3d11va_test
        PRIVATE cxx_std_17
    )
    add_test(
        NAME qtav_hw_d3d11va_contract
        COMMAND qtav_hw_d3d11va_test
    )
    set_tests_properties(
        qtav_hw_d3d11va_contract
        PROPERTIES TIMEOUT 10
    )
endif()

if(TARGET qtav_audio_resample)
    add_executable(
        qtav_audio_resample_test
        audio_resample_player_test.cpp
    )
    target_link_libraries(
        qtav_audio_resample_test
        PRIVATE QtAV::AudioResample
    )
    target_compile_features(qtav_audio_resample_test PRIVATE cxx_std_17)

    add_executable(
        qtav_simulated_audio_sink_player_test
        simulated_audio_sink_player_test.cpp
    )
    target_link_libraries(
        qtav_simulated_audio_sink_player_test
        PRIVATE
            qtav_test_simulated_audio_sink
            QtAV::AudioResample
    )
    target_compile_features(
        qtav_simulated_audio_sink_player_test
        PRIVATE cxx_std_17
    )
    if(TARGET qtav_audio_timestretch)
        target_link_libraries(
            qtav_simulated_audio_sink_player_test
            PRIVATE QtAV::AudioTimeStretch
        )
        target_compile_definitions(
            qtav_simulated_audio_sink_player_test
            PRIVATE QTAV_TEST_HAS_AUDIO_TIMESTRETCH
        )
    endif()

    if(TARGET qtav_audio_file)
        add_executable(
            qtav_audio_file_player_test
            wav_audio_sink_player_test.cpp
        )
        target_link_libraries(
            qtav_audio_file_player_test
            PRIVATE
                QtAV::AudioFile
                QtAV::AudioResample
        )
        target_compile_features(
            qtav_audio_file_player_test
            PRIVATE cxx_std_17
        )
    endif()
endif()

find_program(FFMPEG_EXECUTABLE ffmpeg)
if(FFMPEG_EXECUTABLE)
    set(QTAV_CORE_TEST_MEDIA "${CMAKE_CURRENT_BINARY_DIR}/qtav-core-test.mp4")
    add_test(
        NAME qtav_core_generate_media
        COMMAND
            "${FFMPEG_EXECUTABLE}"
            -hide_banner
            -loglevel error
            -y
            -f lavfi
            -i testsrc2=size=160x90:rate=12
            -f lavfi
            -i sine=frequency=440:sample_rate=48000
            -t 1
            -c:v mpeg4
            -c:a aac
            "${QTAV_CORE_TEST_MEDIA}"
    )
    set_tests_properties(
        qtav_core_generate_media
        PROPERTIES FIXTURES_SETUP qtav_core_media
    )
    add_test(
        NAME qtav_core_playback
        COMMAND qtav_core_playback_test "${QTAV_CORE_TEST_MEDIA}"
    )
    add_test(
        NAME qtav_core_packet_buffer
        COMMAND qtav_core_packet_buffer_test "${QTAV_CORE_TEST_MEDIA}"
    )
    set(
        QTAV_CORE_LIVE_POLICY_TEST_MEDIA
        "${CMAKE_CURRENT_BINARY_DIR}/qtav-core-live-policy-test.mp4"
    )
    add_test(
        NAME qtav_core_generate_live_policy_media
        COMMAND
            "${FFMPEG_EXECUTABLE}"
            -hide_banner
            -loglevel error
            -y
            -f lavfi
            -i testsrc2=size=160x90:rate=60
            -t 4
            -c:v mpeg4
            -g 60
            "${QTAV_CORE_LIVE_POLICY_TEST_MEDIA}"
    )
    set_tests_properties(
        qtav_core_generate_live_policy_media
        PROPERTIES FIXTURES_SETUP qtav_core_live_policy_media
    )
    add_test(
        NAME qtav_core_live_playback_policy
        COMMAND
            qtav_core_live_playback_policy_test
            "${QTAV_CORE_LIVE_POLICY_TEST_MEDIA}"
    )
    set_tests_properties(
        qtav_core_live_playback_policy
        PROPERTIES
            FIXTURES_REQUIRED qtav_core_live_policy_media
            TIMEOUT 20
    )
    if(NOT CMAKE_CROSSCOMPILING)
        find_package(Python3 QUIET COMPONENTS Interpreter)
    endif()
    if(Python3_Interpreter_FOUND)
        set(
            QTAV_CORE_NETWORK_RECOVERY_TEST_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-core-network-recovery-test.mp4"
        )
        set(
            QTAV_CORE_NETWORK_RECOVERY_TEST_AUDIO
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-core-network-recovery-test.m4a"
        )
        add_test(
            NAME qtav_core_generate_network_recovery_media
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i testsrc2=size=640x360:rate=30
                -f lavfi
                -i sine=frequency=440:sample_rate=48000
                -t 8
                -c:v mpeg4
                -b:v 3M
                -g 30
                -c:a aac
                -b:a 128k
                -movflags +faststart
                "${QTAV_CORE_NETWORK_RECOVERY_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_core_generate_network_recovery_media
            PROPERTIES FIXTURES_SETUP qtav_core_network_recovery_media
        )
        add_test(
            NAME qtav_core_generate_network_recovery_audio
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i sine=frequency=660:sample_rate=48000
                -t 8
                -c:a aac
                -b:a 192k
                -movflags +faststart
                "${QTAV_CORE_NETWORK_RECOVERY_TEST_AUDIO}"
        )
        set_tests_properties(
            qtav_core_generate_network_recovery_audio
            PROPERTIES FIXTURES_SETUP qtav_core_network_recovery_media
        )
        add_test(
            NAME qtav_core_network_recovery
            COMMAND
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/network_recovery_server.py"
                --player $<TARGET_FILE:qtav_core_network_recovery_test>
                --media "${QTAV_CORE_NETWORK_RECOVERY_TEST_MEDIA}"
                --audio "${QTAV_CORE_NETWORK_RECOVERY_TEST_AUDIO}"
        )
        set_tests_properties(
            qtav_core_network_recovery
            PROPERTIES
                FIXTURES_REQUIRED qtav_core_network_recovery_media
                TIMEOUT 80
        )
    endif()
    set_tests_properties(
        qtav_core_playback
        qtav_core_packet_buffer
        PROPERTIES
            FIXTURES_REQUIRED qtav_core_media
            TIMEOUT 15
    )
    add_test(
        NAME qtav_core_control
        COMMAND qtav_core_control_test "${QTAV_CORE_TEST_MEDIA}"
    )
    set(
        QTAV_CORE_FRAME_STEP_TEST_MEDIA
        "${CMAKE_CURRENT_BINARY_DIR}/qtav-core-frame-step-test.mp4"
    )
    add_test(
        NAME qtav_core_generate_frame_step_media
        COMMAND
            "${FFMPEG_EXECUTABLE}"
            -hide_banner
            -loglevel error
            -y
            -f lavfi
            -i testsrc2=size=160x90:rate=10
            -f lavfi
            -i sine=frequency=440:sample_rate=48000
            -t 3
            -c:v mpeg4
            -g 10
            -c:a aac
            "${QTAV_CORE_FRAME_STEP_TEST_MEDIA}"
    )
    set_tests_properties(
        qtav_core_generate_frame_step_media
        PROPERTIES FIXTURES_SETUP qtav_core_frame_step_media
    )
    add_test(
        NAME qtav_core_frame_step_seek
        COMMAND
            qtav_core_frame_step_seek_test
            "${QTAV_CORE_FRAME_STEP_TEST_MEDIA}"
    )
    set_tests_properties(
        qtav_core_frame_step_seek
        PROPERTIES
            FIXTURES_REQUIRED qtav_core_frame_step_media
            TIMEOUT 20
    )
    set_tests_properties(
        qtav_core_control
        PROPERTIES
            FIXTURES_REQUIRED qtav_core_media
            TIMEOUT 15
    )
    set(
        QTAV_CORE_MULTITRACK_TEST_MEDIA
        "${CMAKE_CURRENT_BINARY_DIR}/qtav-core-multitrack-test.mkv"
    )
    add_test(
        NAME qtav_core_generate_multitrack_media
        COMMAND
            "${FFMPEG_EXECUTABLE}"
            -hide_banner
            -loglevel error
            -y
            -f lavfi
            -i testsrc2=size=160x90:rate=12
            -f lavfi
            -i color=c=blue:size=96x64:rate=12
            -f lavfi
            -i sine=frequency=440:sample_rate=48000
            -f lavfi
            -i sine=frequency=880:sample_rate=44100
            -f srt
            -i "${CMAKE_CURRENT_SOURCE_DIR}/track-switch-primary.srt"
            -f ass
            -i "${CMAKE_CURRENT_SOURCE_DIR}/track-switch-alternate.ass"
            -map 0:v:0
            -map 1:v:0
            -map 2:a:0
            -map 3:a:0
            -map 4:s:0
            -map 5:s:0
            -t 6
            -c:v mpeg4
            -g 12
            -c:a aac
            -c:s:0 srt
            -c:s:1 ass
            -metadata:s:v:0 language=eng
            -metadata:s:v:1 language=jpn
            -metadata:s:a:0 language=eng
            -metadata:s:a:1 language=jpn
            -metadata:s:s:0 language=eng
            -metadata:s:s:1 language=jpn
            -disposition:v:0 default
            -disposition:v:1 0
            -disposition:a:0 default
            -disposition:a:1 0
            -disposition:s:0 default
            -disposition:s:1 0
            "${QTAV_CORE_MULTITRACK_TEST_MEDIA}"
    )
    set_tests_properties(
        qtav_core_generate_multitrack_media
        PROPERTIES FIXTURES_SETUP qtav_core_multitrack_media
    )
    add_test(
        NAME qtav_core_track_switch
        COMMAND
            qtav_core_track_switch_test
            "${QTAV_CORE_MULTITRACK_TEST_MEDIA}"
    )
    add_test(
        NAME qtav_core_external_tracks
        COMMAND
            qtav_core_external_track_test
            "${QTAV_CORE_TEST_MEDIA}"
            "${QTAV_CORE_MULTITRACK_TEST_MEDIA}"
            "${QTAV_CORE_MULTITRACK_TEST_MEDIA}"
    )
    add_test(
        NAME qtav_core_external_tracks_without_primary_decoder
        COMMAND
            qtav_core_external_track_test
            "${QTAV_CORE_TEST_MEDIA}"
            "${QTAV_CORE_MULTITRACK_TEST_MEDIA}"
            "${QTAV_CORE_MULTITRACK_TEST_MEDIA}"
            --disable-primary-video
    )
    if(TARGET qtav_subtitle_libass_test)
        add_test(
            NAME qtav_subtitle_libass_render
            COMMAND
                qtav_subtitle_libass_test
                "${QTAV_CORE_MULTITRACK_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_subtitle_libass_render
            PROPERTIES
                FIXTURES_REQUIRED qtav_core_multitrack_media
                TIMEOUT 30
        )
    endif()
    set_tests_properties(
        qtav_core_track_switch
        PROPERTIES
            FIXTURES_REQUIRED qtav_core_multitrack_media
            TIMEOUT 30
    )
    set_tests_properties(
        qtav_core_external_tracks
        qtav_core_external_tracks_without_primary_decoder
        PROPERTIES
            FIXTURES_REQUIRED
                "qtav_core_media;qtav_core_multitrack_media"
            TIMEOUT 45
    )
    add_test(
        NAME qtav_core_audio_sink_player
        COMMAND qtav_core_audio_sink_player_test "${QTAV_CORE_TEST_MEDIA}"
    )
    add_test(
        NAME qtav_core_audio_time_stretch_player
        COMMAND
            qtav_core_audio_time_stretch_player_test
            "${QTAV_CORE_TEST_MEDIA}"
    )
    add_test(
        NAME qtav_core_processing_player
        COMMAND
            qtav_core_processing_player_test
            "${QTAV_CORE_TEST_MEDIA}"
    )
    set_tests_properties(
        qtav_core_audio_sink_player
        qtav_core_audio_time_stretch_player
        qtav_core_processing_player
        PROPERTIES
            FIXTURES_REQUIRED qtav_core_media
            TIMEOUT 15
    )
    add_test(
        NAME qtav_core_hardware_decode_device_player
        COMMAND
            qtav_core_hardware_decode_device_test
            "${QTAV_CORE_TEST_MEDIA}"
    )
    set_tests_properties(
        qtav_core_hardware_decode_device_player
        PROPERTIES
            FIXTURES_REQUIRED qtav_core_media
            TIMEOUT 15
    )

    if(TARGET qtav_output_d3d11_test)
        add_test(
            NAME qtav_output_d3d11_composition
            COMMAND
                qtav_output_d3d11_test
                "${QTAV_CORE_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_output_d3d11_composition
            PROPERTIES
                FIXTURES_REQUIRED qtav_core_media
                TIMEOUT 15
        )
    endif()

    if(TARGET qtav_audio_wasapi
       AND TARGET qtav_audio_resample
       AND TARGET qtav_interop_d3d11
       AND TARGET qtav_hw_d3d11va
       AND TARGET qtav_render_d3d11
       AND TARGET qtav_core_console)
        set(
            QTAV_WINDOWS_NATIVE_AV_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-windows-native-av.mp4"
        )
        add_test(
            NAME qtav_windows_native_av_generate_media
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i
                "color=c=red:s=160x90:r=12:d=1,drawbox=x=80:y=0:w=80:h=90:color=blue:t=fill"
                -f lavfi
                -i sine=frequency=440:sample_rate=48000
                -t 1
                -c:v h264_mf
                -pix_fmt yuv420p
                -g 12
                -c:a aac
                "${QTAV_WINDOWS_NATIVE_AV_MEDIA}"
        )
        set_tests_properties(
            qtav_windows_native_av_generate_media
            PROPERTIES FIXTURES_SETUP qtav_windows_native_av_media
        )
        add_test(
            NAME qtav_windows_native_av
            COMMAND
                qtav_core_console
                "${QTAV_WINDOWS_NATIVE_AV_MEDIA}"
        )
        set_tests_properties(
            qtav_windows_native_av
            PROPERTIES
                FIXTURES_REQUIRED qtav_windows_native_av_media
                TIMEOUT 15
                SKIP_RETURN_CODE 77
                ENVIRONMENT QTAV_CORE_REQUIRE_NATIVE_WINDOWS_AV=1
        )
    endif()

    if(TARGET qtav_simulated_audio_sink_player_test)
        add_test(
            NAME qtav_simulated_audio_sink_player
            COMMAND
                qtav_simulated_audio_sink_player_test
                "${QTAV_CORE_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_simulated_audio_sink_player
            PROPERTIES
                FIXTURES_REQUIRED qtav_core_media
                TIMEOUT 15
        )
    endif()

    if(TARGET qtav_render_cpu_test)
        set(
            QTAV_RENDER_CPU_TEST_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-render-cpu-test.nut"
        )
        add_test(
            NAME qtav_render_cpu_generate_media
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i
                "nullsrc=size=4x2:rate=1,geq=r=17:g=34:b=51,format=rgb24"
                -frames:v 1
                -c:v rawvideo
                -pix_fmt rgb24
                "${QTAV_RENDER_CPU_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_render_cpu_generate_media
            PROPERTIES FIXTURES_SETUP qtav_render_cpu_media
        )
        add_test(
            NAME qtav_render_cpu_conversion
            COMMAND qtav_render_cpu_test "${QTAV_RENDER_CPU_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_render_cpu_conversion
            PROPERTIES
                FIXTURES_REQUIRED qtav_render_cpu_media
                TIMEOUT 15
        )
    endif()

    if(TARGET qtav_render_vulkan_test)
        set(
            QTAV_RENDER_VULKAN_TEST_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-vulkan-yuv420p.nut"
        )
        add_test(
            NAME qtav_render_vulkan_generate_media
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -loglevel error
                -y
                -f lavfi
                -i
                "color=c=red:s=160x90:r=1:d=1,drawbox=x=80:y=0:w=80:h=90:color=blue:t=fill,format=yuv420p"
                -frames:v 1
                -c:v rawvideo
                -pix_fmt yuv420p
                "${QTAV_RENDER_VULKAN_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_render_vulkan_generate_media
            PROPERTIES FIXTURES_SETUP qtav_render_vulkan_media
        )
        add_test(
            NAME qtav_render_vulkan_offscreen
            COMMAND
                qtav_render_vulkan_test
                "${QTAV_RENDER_VULKAN_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_render_vulkan_offscreen
            PROPERTIES
                FIXTURES_REQUIRED qtav_render_vulkan_media
                TIMEOUT 20
                SKIP_RETURN_CODE 77
        )
    endif()

    if(TARGET qtav_render_d3d11_test)
        set(
            QTAV_RENDER_D3D11_RGB_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-d3d11-rgb.nut"
        )
        set(
            QTAV_RENDER_D3D11_YUV_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-d3d11-yuv420p.nut"
        )
        set(
            QTAV_RENDER_D3D11_NV12_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-d3d11-nv12.nut"
        )
        add_test(
            NAME qtav_render_d3d11_generate_rgb
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -loglevel error
                -y
                -f lavfi
                -i
                "color=c=red:s=4x2:r=1:d=1,drawbox=x=2:y=0:w=2:h=2:color=blue:t=fill,format=rgb24"
                -frames:v 1
                -c:v rawvideo
                -pix_fmt rgb24
                "${QTAV_RENDER_D3D11_RGB_MEDIA}"
        )
        add_test(
            NAME qtav_render_d3d11_generate_yuv
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -loglevel error
                -y
                -f lavfi
                -i
                "color=c=red:s=4x2:r=1:d=1,drawbox=x=2:y=0:w=2:h=2:color=blue:t=fill,format=yuv420p"
                -frames:v 1
                -c:v rawvideo
                -pix_fmt yuv420p
                "${QTAV_RENDER_D3D11_YUV_MEDIA}"
        )
        add_test(
            NAME qtav_render_d3d11_generate_nv12
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -loglevel error
                -y
                -f lavfi
                -i
                "color=c=red:s=4x2:r=1:d=1,drawbox=x=2:y=0:w=2:h=2:color=blue:t=fill,format=nv12"
                -frames:v 1
                -c:v rawvideo
                -pix_fmt nv12
                "${QTAV_RENDER_D3D11_NV12_MEDIA}"
        )
        set_tests_properties(
            qtav_render_d3d11_generate_rgb
            qtav_render_d3d11_generate_yuv
            qtav_render_d3d11_generate_nv12
            PROPERTIES FIXTURES_SETUP qtav_render_d3d11_media
        )
        add_test(
            NAME qtav_render_d3d11_software_frames
            COMMAND
                qtav_render_d3d11_test
                "${QTAV_RENDER_D3D11_RGB_MEDIA}"
                "${QTAV_RENDER_D3D11_YUV_MEDIA}"
                "${QTAV_RENDER_D3D11_NV12_MEDIA}"
        )
        set_tests_properties(
            qtav_render_d3d11_software_frames
            PROPERTIES
                FIXTURES_REQUIRED qtav_render_d3d11_media
                TIMEOUT 20
        )
    endif()

    if(TARGET qtav_hw_d3d11va_test)
        set(
            QTAV_HW_D3D11VA_TEST_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-d3d11va-test.mp4"
        )
        add_test(
            NAME qtav_hw_d3d11va_generate_media
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i testsrc2=size=160x90:rate=12
                -t 2
                -c:v h264_mf
                -pix_fmt yuv420p
                -g 12
                "${QTAV_HW_D3D11VA_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_hw_d3d11va_generate_media
            PROPERTIES FIXTURES_SETUP qtav_hw_d3d11va_media
        )
        add_test(
            NAME qtav_hw_d3d11va_lifecycle
            COMMAND
                qtav_hw_d3d11va_test
                "${QTAV_HW_D3D11VA_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_hw_d3d11va_lifecycle
            PROPERTIES
                FIXTURES_REQUIRED qtav_hw_d3d11va_media
                TIMEOUT 20
        )
    endif()

    if(TARGET qtav_interop_d3d11_test)
        set(
            QTAV_INTEROP_D3D11_TEST_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-d3d11-interop-test.mp4"
        )
        set(
            QTAV_INTEROP_D3D11_MAIN10_TEST_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-d3d11-interop-main10-test.mp4"
        )
        add_test(
            NAME qtav_interop_d3d11_generate_media
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i
                "color=c=red:s=160x90:r=4:d=1,drawbox=x=80:y=0:w=80:h=90:color=blue:t=fill,format=yuv420p"
                -frames:v 4
                -c:v h264_mf
                -pix_fmt yuv420p
                -color_range tv
                -colorspace bt709
                -color_primaries bt709
                -color_trc bt709
                -g 4
                "${QTAV_INTEROP_D3D11_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_interop_d3d11_generate_media
            PROPERTIES FIXTURES_SETUP qtav_interop_d3d11_media
        )
        add_test(
            NAME qtav_interop_d3d11_generate_main10_media
            COMMAND
                "${CMAKE_COMMAND}"
                "-DFFMPEG_EXECUTABLE=${FFMPEG_EXECUTABLE}"
                "-DOUTPUT_FILE=${QTAV_INTEROP_D3D11_MAIN10_TEST_MEDIA}"
                -P
                "${CMAKE_CURRENT_SOURCE_DIR}/generate_d3d12_main10_media.cmake"
        )
        set_tests_properties(
            qtav_interop_d3d11_generate_main10_media
            PROPERTIES
                FIXTURES_SETUP qtav_interop_d3d11_main10_media
        )
        add_test(
            NAME qtav_interop_d3d11_gpu_copy
            COMMAND
                qtav_interop_d3d11_test
                "${QTAV_INTEROP_D3D11_TEST_MEDIA}"
                "${QTAV_INTEROP_D3D11_MAIN10_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_interop_d3d11_gpu_copy
            PROPERTIES
                FIXTURES_REQUIRED
                    "qtav_interop_d3d11_media;qtav_interop_d3d11_main10_media"
                TIMEOUT 30
        )
        add_test(
            NAME qtav_interop_d3d11_direct_decoder_sampling
            COMMAND
                qtav_interop_d3d11_test
                "${QTAV_INTEROP_D3D11_TEST_MEDIA}"
                "${QTAV_INTEROP_D3D11_MAIN10_TEST_MEDIA}"
                --direct-decoder-sampling
        )
        set_tests_properties(
            qtav_interop_d3d11_direct_decoder_sampling
            PROPERTIES
                FIXTURES_REQUIRED
                    "qtav_interop_d3d11_media;qtav_interop_d3d11_main10_media"
                TIMEOUT 30
        )
    endif()

    if(TARGET qtav_audio_resample_test)
        set(
            QTAV_AUDIO_RESAMPLE_TEST_MEDIA
            "${CMAKE_CURRENT_BINARY_DIR}/qtav-audio-resample-test.wav"
        )
        add_test(
            NAME qtav_audio_resample_generate_media
            COMMAND
                "${FFMPEG_EXECUTABLE}"
                -hide_banner
                -loglevel error
                -y
                -f lavfi
                -i "aevalsrc=0.5:sample_rate=8000:duration=1"
                -c:a pcm_s16le
                "${QTAV_AUDIO_RESAMPLE_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_audio_resample_generate_media
            PROPERTIES FIXTURES_SETUP qtav_audio_resample_media
        )
        add_test(
            NAME qtav_audio_resample_player
            COMMAND
                qtav_audio_resample_test
                "${QTAV_AUDIO_RESAMPLE_TEST_MEDIA}"
        )
        set_tests_properties(
            qtav_audio_resample_player
            PROPERTIES
                FIXTURES_REQUIRED qtav_audio_resample_media
                TIMEOUT 15
        )

        if(TARGET qtav_audio_file_player_test)
            add_test(
                NAME qtav_audio_file_player
                COMMAND
                    qtav_audio_file_player_test
                    "${QTAV_AUDIO_RESAMPLE_TEST_MEDIA}"
                    "${CMAKE_CURRENT_BINARY_DIR}/qtav-audio-file-player.wav"
            )
            set_tests_properties(
                qtav_audio_file_player
                PROPERTIES
                    FIXTURES_REQUIRED qtav_audio_resample_media
                    TIMEOUT 15
            )
        endif()
    endif()
endif()
