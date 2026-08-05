// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mobile_video_renderer.h>
#include <qtav/ohos_opengl_video_renderer.h>
#include <qtav/ohos_vulkan_video_renderer.h>

#include <memory>
#include <string>
#include <utility>

extern "C" __attribute__((visibility("default")))
int qtav_ohos_render_install_consumer()
{
    qtav::OHOSOpenGLVideoRenderer renderer(
        qtav::OpenGLOutputPreference::SdrOnly);
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
    const qtav::BorrowedOHOSVulkanContext emptyVulkanContext;
    return capabilities.customViewport
            && capabilities.rotation
            && capabilities.ownedContext
            && capabilities.ownedSurface
            && renderer.outputColorSpace()
                == qtav::OpenGLOutputColorSpace::SdrSrgb
            && !renderer.hdrOutputActive()
            && renderer.colorComponentBits() == 0
            && selector.selectedAPI() == qtav::MobileRenderAPI::None
            && !emptyVulkanContext.isValid()
            && std::string(
                   qtav::mobileRenderAPIName(
                       qtav::MobileRenderAPI::OpenGLES))
                == "opengl-es"
        ? 0
        : 1;
}
