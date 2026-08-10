// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/aaudio_audio_sink.h>
#include <qtav/android_opengl_video_renderer.h>
#include <qtav/mediacodec_hardware_decoder.h>
#include <qtav/mediacodec_opengl_interop.h>
#include <qtav/mediacodec_vulkan_interop.h>
#include <qtav/mobile_video_renderer.h>
#include <qtav/version.h>
#include <qtav/volume_audio_frame_processor.h>

#include <memory>
#include <string>
#include <utility>

static_assert(QTAV_CORE_VERSION_MAJOR == 2);
static_assert(QTAV_CORE_VERSION_MINOR == 0);
static_assert(QTAV_CORE_VERSION_PATCH == 0);
static_assert(qtav::coreVersion == qtav::Version { 2, 0, 0 });
static_assert(qtav::coreVersionString == "2.0.0");

extern "C" __attribute__((visibility("default")))
int qtav_android_opengl_install_consumer()
{
    qtav::AndroidOpenGLVideoRenderer renderer(
        qtav::OpenGLOutputPreference::PreferHdr);
    qtav::AAudioAudioSink audioSink;
    qtav::VolumeAudioFrameProcessor audioFilter(0.5);
    const qtav::MediaCodecSurface emptyMediaCodecSurface;
    const qtav::HardwareDecodeConfig mediaCodecConfig =
        qtav::mediaCodecHardwareDecodeConfig(
            emptyMediaCodecSurface);
    qtav::MediaCodecVulkanInteropConfig interopConfig;
    qtav::MediaCodecOpenGLInteropConfig openGLInteropConfig;
    qtav::MobileRendererSelectorConfig selectorConfig;
    selectorConfig.openGLES = [] {
        return qtav::MobileRendererCandidate {
            std::make_shared<qtav::AndroidOpenGLVideoRenderer>(),
            {},
        };
    };
    qtav::MobileVideoRendererSelector selector(
        std::move(selectorConfig));
    selector.setHardwareFrameFallbackCallback(
        [](const qtav::MobileHardwareFrameFallbackEvent&) {
            return qtav::MobileHardwareFrameFallbackDecision {
                qtav::MobileHardwareFrameFallbackRoute::NoVideo,
                {},
            };
        });
    const qtav::VideoRenderCapabilities capabilities =
        renderer.capabilities();
    const qtav::AudioSinkCapabilities audioCapabilities =
        audioSink.capabilities();
    return capabilities.customViewport && capabilities.rotation
            && audioCapabilities.supportsPause
            && audioCapabilities.hasDeviceClock
            && audioCapabilities.maximumChannels == 2
            && audioFilter.gain() == 0.5
            && renderer.outputColorSpace()
                == qtav::OpenGLOutputColorSpace::SdrSrgb
            && !renderer.hdrOutputActive()
            && renderer.colorComponentBits() == 0
            && selector.selectedAPI() == qtav::MobileRenderAPI::None
            && selector.hardwareFrameFallbackRoute()
                == qtav::MobileHardwareFrameFallbackRoute::None
            && std::string(
                   qtav::mobileHardwareFrameFallbackRouteName(
                       qtav::MobileHardwareFrameFallbackRoute::
                           OpenGLESInterop))
                == "opengl-es-interop"
            && mediaCodecConfig.deviceType
                == qtav::HardwareDeviceType::MediaCodec
            && mediaCodecConfig.decoderWrapper == "mediacodec"
            && mediaCodecConfig.requireSuppliedDevice
            && !mediaCodecConfig.device
            && interopConfig.maximumImages == 5
            && openGLInteropConfig.maximumPendingFrames == 4
        ? 0
        : 1;
}
