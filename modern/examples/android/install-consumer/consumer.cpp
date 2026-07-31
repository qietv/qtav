// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/aaudio_audio_sink.h>
#include <qtav/android_opengl_video_renderer.h>
#include <qtav/mediacodec_hardware_decoder.h>
#include <qtav/mediacodec_opengl_interop.h>
#include <qtav/mediacodec_vulkan_interop.h>
#include <qtav/mobile_video_renderer.h>

#include <memory>
#include <utility>

extern "C" __attribute__((visibility("default")))
int qtav_android_opengl_install_consumer()
{
    qtav::AndroidOpenGLVideoRenderer renderer(
        qtav::OpenGLOutputPreference::PreferHdr);
    qtav::AAudioAudioSink audioSink;
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
    const qtav::VideoRenderCapabilities capabilities =
        renderer.capabilities();
    const qtav::AudioSinkCapabilities audioCapabilities =
        audioSink.capabilities();
    return capabilities.customViewport && capabilities.rotation
            && audioCapabilities.supportsPause
            && audioCapabilities.hasDeviceClock
            && audioCapabilities.maximumChannels == 2
            && renderer.outputColorSpace()
                == qtav::OpenGLOutputColorSpace::SdrSrgb
            && !renderer.hdrOutputActive()
            && renderer.colorComponentBits() == 0
            && selector.selectedAPI() == qtav::MobileRenderAPI::None
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
