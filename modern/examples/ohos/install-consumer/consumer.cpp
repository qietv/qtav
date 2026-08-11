// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mobile_video_renderer.h>
#include <qtav/ohcodec_hardware_decoder.h>
#include <qtav/ohcodec_opengl_interop.h>
#include <qtav/ohcodec_vulkan_interop.h>
#include <qtav/ohos_opengl_video_renderer.h>
#include <qtav/ohos_vulkan_video_renderer.h>
#include <qtav/ohaudio_audio_sink.h>
#include <qtav/swresample_audio_converter.h>
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
int qtav_ohos_render_install_consumer()
{
    qtav::OHOSOpenGLVideoRenderer renderer(
        qtav::OpenGLOutputPreference::SdrOnly);
    qtav::OHAudioAudioSink audioSink;
    qtav::SwresampleAudioConverter audioConverter;
    qtav::VolumeAudioFrameProcessor audioFilter(0.5);
    qtav::MobileRendererSelectorConfig selectorConfig;
    selectorConfig.openGLES = [] {
        return qtav::MobileRendererCandidate {
            std::make_shared<qtav::OHOSOpenGLVideoRenderer>(
                qtav::OpenGLOutputPreference::SdrOnly),
            "OHOS EGL candidate",
        };
    };
    qtav::MobileVideoRendererSelector selector(
        std::move(selectorConfig));
    const qtav::VideoRenderCapabilities capabilities =
        renderer.capabilities();
    const qtav::AudioSinkCapabilities audioCapabilities =
        audioSink.capabilities();
    const qtav::BorrowedOHOSVulkanContext emptyVulkanContext;
    const qtav::OHCodecSurface emptyOHCodecSurface;
    const qtav::HardwareDecodeConfig ohCodecConfig =
        qtav::ohCodecHardwareDecodeConfig(emptyOHCodecSurface);
    qtav::OHCodecFrame emptyOHCodecFrame;
    const qtav::OHCodecOpenGLInteropConfig ohCodecOpenGLConfig;
    qtav::OHCodecOpenGLInterop ohCodecOpenGLInterop(
        ohCodecOpenGLConfig);
    const qtav::OHCodecOpenGLInteropStatistics
        ohCodecOpenGLStatistics = ohCodecOpenGLInterop.statistics();
    qtav::OHCodecVulkanInteropConfig ohCodecVulkanConfig;
    qtav::OHCodecVulkanInterop ohCodecVulkanInterop(
        {},
        ohCodecVulkanConfig);
    const qtav::OHCodecVulkanInteropStatistics
        ohCodecVulkanStatistics = ohCodecVulkanInterop.statistics();
    const qtav::OHCodecVulkanNativeBufferObservation
        ohCodecVulkanObservation =
            ohCodecVulkanInterop.nativeBufferObservation();
    return capabilities.customViewport
            && capabilities.rotation
            && capabilities.ownedContext
            && capabilities.ownedSurface
            && renderer.outputColorSpace()
                == qtav::OpenGLOutputColorSpace::SdrSrgb
            && !renderer.hdrOutputActive()
            && renderer.colorComponentBits() == 0
            && selector.selectedAPI() == qtav::MobileRenderAPI::None
            && audioCapabilities.hasDeviceClock
            && audioCapabilities.supportsPause
            && audioCapabilities.maximumChannels == 2
            && audioFilter.gain() == 0.5
            && audioConverter.open(
                { 48'000, 1, qtav::SampleFormat::Float, "mono" },
                { 48'000, 2, qtav::SampleFormat::Float, "stereo" })
                .success
            && !emptyVulkanContext.isValid()
            && ohCodecConfig.deviceType
                == qtav::HardwareDeviceType::OHCodec
            && !ohCodecConfig.device
            && ohCodecConfig.requireSuppliedDevice
            && ohCodecConfig.decoderWrapper == "ohcodec"
            && !emptyOHCodecFrame
            && !emptyOHCodecFrame.drop()
            && !ohCodecOpenGLInterop.surface()
            && ohCodecOpenGLStatistics.cpuMapCalls == 0
            && ohCodecOpenGLStatistics.softwareTransferCalls == 0
            && ohCodecOpenGLStatistics.stagingCopies == 0
            && ohCodecOpenGLStatistics.rendererUploads == 0
            && ohCodecOpenGLStatistics.rawYcbcrImages == 0
            && ohCodecOpenGLStatistics.implicitRgbImages == 0
            && ohCodecOpenGLStatistics
                    .microsecondTimestampsNormalized
                == 0
            && !ohCodecVulkanInterop
            && ohCodecVulkanStatistics.cpuMapCalls == 0
            && ohCodecVulkanStatistics.softwareTransferCalls == 0
            && ohCodecVulkanStatistics.stagingCopies == 0
            && ohCodecVulkanStatistics.rendererUploads == 0
            && ohCodecVulkanStatistics.normalizationPasses == 0
            && ohCodecVulkanObservation.nativeWidth == 0
            && ohCodecVulkanObservation.nativeHeight == 0
            && ohCodecVulkanObservation.nativeUsage == 0
            && ohCodecVulkanObservation.formatFeatures == 0
            && std::string(
                   qtav::mobileRenderAPIName(
                       qtav::MobileRenderAPI::OpenGLES))
                == "opengl-es"
        ? 0
        : 1;
}
