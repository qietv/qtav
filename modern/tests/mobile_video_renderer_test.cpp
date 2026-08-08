// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mobile_video_renderer.h>

#include "frame_internal.h"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct RenderResult {
    bool succeeded = true;
    qtav::VideoRenderEventType eventType =
        qtav::VideoRenderEventType::RedrawRequested;
    std::string detail;
    bool emitEvent = true;
    qtav::VideoRenderAttemptStatus detailedStatus =
        qtav::VideoRenderAttemptStatus::RetryAfterBackoff;
    bool useDetailedStatus = false;
    std::uint32_t retryAfterMilliseconds = 0;
};

struct RendererBehavior {
    bool openSucceeded = true;
    bool configureSucceeded = true;
    std::vector<qtav::HardwareDeviceType> hardwareDevices;
    std::deque<RenderResult> renderResults;
    int openCount = 0;
    int configureCount = 0;
    int renderCount = 0;
    int closeCount = 0;
};

class MockVideoRenderer final : public qtav::VideoRenderAPI {
public:
    explicit MockVideoRenderer(
        std::shared_ptr<RendererBehavior> behavior)
        : behavior_(std::move(behavior))
    {
    }

    qtav::VideoRenderCapabilities capabilities() const override
    {
        qtav::VideoRenderCapabilities result;
        result.softwareFormats = { qtav::PixelFormat::YUV420P };
        result.hardwareDevices = behavior_->hardwareDevices;
        return result;
    }

    void setEventCallback(EventCallback callback) override
    {
        callback_ = std::move(callback);
    }

    bool open(const qtav::VideoRenderConfig&) override
    {
        ++behavior_->openCount;
        if (!behavior_->openSucceeded && callback_) {
            callback_({
                qtav::VideoRenderEventType::Error,
                "scripted open failure",
            });
        }
        return behavior_->openSucceeded;
    }

    bool configure(const qtav::VideoRenderConfig&) override
    {
        ++behavior_->configureCount;
        if (!behavior_->configureSucceeded && callback_) {
            callback_({
                qtav::VideoRenderEventType::Error,
                "scripted configure failure",
            });
        }
        return behavior_->configureSucceeded;
    }

    qtav::VideoRenderAttemptResult renderDetailed(
        const qtav::VideoFrame&) override
    {
        ++behavior_->renderCount;
        RenderResult result;
        if (!behavior_->renderResults.empty()) {
            result = std::move(behavior_->renderResults.front());
            behavior_->renderResults.pop_front();
        }
        if (!result.succeeded && result.emitEvent && callback_) {
            callback_({ result.eventType, std::move(result.detail) });
        }
        if (result.useDetailedStatus) {
            return {
                result.detailedStatus,
                result.retryAfterMilliseconds,
                std::move(result.detail),
            };
        }
        return {
            result.succeeded
                ? qtav::VideoRenderAttemptStatus::Presented
                : qtav::VideoRenderAttemptStatus::RetryAfterBackoff,
            result.succeeded ? 0U : 1U,
            std::move(result.detail),
        };
    }

    bool render(const qtav::VideoFrame& frame) override
    {
        return renderDetailed(frame).frameConsumed();
    }

    void close() noexcept override
    {
        ++behavior_->closeCount;
    }

private:
    std::shared_ptr<RendererBehavior> behavior_;
    EventCallback callback_;
};

struct FactoryScript {
    std::deque<std::shared_ptr<RendererBehavior>> behaviors;
    std::string unavailableDetail;
    int calls = 0;

    qtav::MobileRendererCandidate create()
    {
        ++calls;
        if (behaviors.empty()) {
            return { {}, unavailableDetail };
        }
        auto behavior = std::move(behaviors.front());
        behaviors.pop_front();
        return {
            std::make_shared<MockVideoRenderer>(std::move(behavior)),
            {},
        };
    }
};

std::shared_ptr<RendererBehavior> behavior(
    std::initializer_list<RenderResult> results = {})
{
    auto result = std::make_shared<RendererBehavior>();
    result->renderResults.assign(results.begin(), results.end());
    return result;
}

RenderResult detailedAttempt(
    qtav::VideoRenderAttemptStatus status,
    std::string detail = {},
    std::uint32_t retryAfterMilliseconds = 0)
{
    RenderResult result;
    result.succeeded =
        status == qtav::VideoRenderAttemptStatus::Presented;
    result.detail = std::move(detail);
    result.emitEvent = false;
    result.detailedStatus = status;
    result.useDetailedStatus = true;
    result.retryAfterMilliseconds = retryAfterMilliseconds;
    return result;
}

class MockHardwareFrameData final : public qtav::HardwareFrameData {
public:
    MockHardwareFrameData(
        std::uintptr_t surface,
        std::uint32_t generation,
        int* mapCalls)
        : surface_(surface)
        , generation_(generation)
        , mapCalls_(mapCalls)
    {
    }

    qtav::HardwareDeviceType deviceType() const noexcept override
    {
        return qtav::HardwareDeviceType::MediaCodec;
    }
    int width() const noexcept override { return 160; }
    int height() const noexcept override { return 90; }
    qtav::PixelFormat softwareFormat() const noexcept override
    {
        return qtav::PixelFormat::Native;
    }
    qtav::NativeHandle nativeHandle(
        qtav::HardwareHandleType type) const noexcept override
    {
        if (type == qtav::HardwareHandleType::Surface) {
            return { type, surface_, generation_ };
        }
        if (type == qtav::HardwareHandleType::Frame) {
            return { type, surface_ + 1, generation_ };
        }
        return { type, 0, 0 };
    }
    bool isMappable(qtav::HardwareMapMode) const noexcept override
    {
        return true;
    }
    std::shared_ptr<qtav::HardwareFrameMapping> map(
        qtav::HardwareMapMode) const override
    {
        if (mapCalls_) {
            ++*mapCalls_;
        }
        return {};
    }

private:
    std::uintptr_t surface_ = 0;
    std::uint32_t generation_ = 0;
    int* mapCalls_ = nullptr;
};

qtav::VideoFrame hardwareFrame(
    std::uintptr_t surface,
    std::uint32_t generation,
    int& mapCalls)
{
    return qtav::detail::FrameFactory::hardware(
        qtav::HardwareFrame(
            std::make_shared<MockHardwareFrameData>(
                surface,
                generation,
                &mapCalls)));
}

qtav::MobileRendererSelectorConfig selectorConfig(
    const std::shared_ptr<FactoryScript>& vulkan,
    const std::shared_ptr<FactoryScript>& openGLES,
    std::size_t maximumRecoveryAttempts = 2)
{
    return {
        [vulkan] { return vulkan->create(); },
        [openGLES] { return openGLES->create(); },
        maximumRecoveryAttempts,
    };
}

qtav::VideoRenderConfig renderConfig()
{
    qtav::VideoRenderConfig result;
    result.surfaceSize = { 640, 360 };
    return result;
}

void testPreferredVulkan()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->behaviors.push_back(behavior());

    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    expect(selector.open(renderConfig()), "Vulkan startup failed");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::Vulkan,
        "Vulkan was not preferred");
    expect(!selector.usingFallback(), "Vulkan was marked as fallback");
    expect(
        selector.render(qtav::VideoFrame {}),
        "Vulkan render failed");
    expect(openGLES->calls == 0, "OpenGL ES was probed unnecessarily");
}

void testPreferredOpenGLES()
{
    {
        auto vulkan = std::make_shared<FactoryScript>();
        auto openGLES = std::make_shared<FactoryScript>();
        openGLES->behaviors.push_back(behavior());
        auto config = selectorConfig(vulkan, openGLES);
        config.preferredAPI = qtav::MobileRenderAPI::OpenGLES;

        qtav::MobileVideoRendererSelector selector(std::move(config));
        expect(selector.open(renderConfig()), "OpenGL ES startup failed");
        expect(
            selector.selectedAPI() == qtav::MobileRenderAPI::OpenGLES,
            "The configured OpenGL ES preference was ignored");
        expect(
            !selector.usingFallback(),
            "The preferred OpenGL ES backend was marked as fallback");
        expect(vulkan->calls == 0, "Vulkan was probed unnecessarily");
    }

    {
        auto vulkan = std::make_shared<FactoryScript>();
        auto openGLES = std::make_shared<FactoryScript>();
        openGLES->unavailableDetail = "No compatible EGLConfig";
        vulkan->behaviors.push_back(behavior());
        auto config = selectorConfig(vulkan, openGLES);
        config.preferredAPI = qtav::MobileRenderAPI::OpenGLES;

        qtav::MobileVideoRendererSelector selector(std::move(config));
        expect(
            selector.open(renderConfig()),
            "Vulkan did not handle unavailable preferred OpenGL ES");
        expect(
            selector.selectedAPI() == qtav::MobileRenderAPI::Vulkan,
            "The alternate Vulkan backend was not selected");
        expect(
            selector.usingFallback(),
            "The alternate Vulkan backend was not marked as fallback");
    }
}

void testVulkanUnavailableAndInitialFailure()
{
    {
        auto vulkan = std::make_shared<FactoryScript>();
        auto openGLES = std::make_shared<FactoryScript>();
        vulkan->unavailableDetail = "Vulkan loader unavailable";
        openGLES->behaviors.push_back(behavior());

        qtav::MobileVideoRendererSelector selector(
            selectorConfig(vulkan, openGLES));
        expect(
            selector.open(renderConfig()),
            "OpenGL ES did not handle unavailable Vulkan");
        expect(
            selector.selectedAPI()
                == qtav::MobileRenderAPI::OpenGLES,
            "Unavailable Vulkan did not select OpenGL ES");
        expect(selector.usingFallback(), "Fallback was not observable");
    }

    {
        auto vulkan = std::make_shared<FactoryScript>();
        auto openGLES = std::make_shared<FactoryScript>();
        auto failedOpen = behavior();
        failedOpen->openSucceeded = false;
        vulkan->behaviors.push_back(failedOpen);
        openGLES->behaviors.push_back(behavior());

        qtav::MobileVideoRendererSelector selector(
            selectorConfig(vulkan, openGLES));
        expect(
            selector.open(renderConfig()),
            "OpenGL ES did not handle initial Vulkan failure");
        expect(
            selector.selectedAPI()
                == qtav::MobileRenderAPI::OpenGLES,
            "Initial Vulkan failure did not select OpenGL ES");
        expect(failedOpen->closeCount == 1, "Failed Vulkan was not closed");
    }
}

void testRecoverableVulkanRecreation()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->behaviors.push_back(
        behavior({
            detailedAttempt(
                qtav::VideoRenderAttemptStatus::SurfaceLost,
                "scripted recoverable surface loss"),
        }));
    vulkan->behaviors.push_back(behavior());

    std::vector<qtav::MobileRendererSelectionEvent> events;
    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    selector.setSelectionCallback(
        [&events](const qtav::MobileRendererSelectionEvent& event) {
            events.push_back(event);
        });
    expect(selector.open(renderConfig()), "Vulkan startup failed");
    expect(
        selector.render(qtav::VideoFrame {}),
        "Recoverable Vulkan recreation failed");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::Vulkan,
        "Recoverable failure changed graphics API");
    expect(vulkan->calls == 2, "Vulkan was not recreated exactly once");
    expect(openGLES->calls == 0, "Recoverable failure probed OpenGL ES");
    expect(
        events.size() == 2
            && events.back().type
                == qtav::MobileRendererSelectionEventType::Recovered,
        "Vulkan recovery event was not emitted");
}

void testFatalOneWayFallback()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->behaviors.push_back(
        behavior({
            detailedAttempt(
                qtav::VideoRenderAttemptStatus::FatalError,
                "VK_ERROR_DEVICE_LOST"),
        }));
    openGLES->behaviors.push_back(behavior());
    openGLES->behaviors.push_back(behavior());

    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    expect(selector.open(renderConfig()), "Vulkan startup failed");
    const std::uint64_t session = selector.sessionGeneration();
    expect(
        selector.render(qtav::VideoFrame {}),
        "Fatal Vulkan failure did not retry through OpenGL ES");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::OpenGLES,
        "Fatal Vulkan failure did not switch to OpenGL ES");
    expect(selector.usingFallback(), "One-way fallback was not recorded");
    expect(
        selector.sessionGeneration() == session,
        "Fallback started a new renderer session");

    selector.suspendSurface();
    expect(
        selector.recreateSurface(),
        "OpenGL ES surface recreation failed after fallback");
    expect(vulkan->calls == 1, "Vulkan was reprobed in the same session");
    expect(openGLES->calls == 2, "OpenGL ES was not recreated");

    selector.close();
    vulkan->behaviors.push_back(behavior());
    expect(selector.open(renderConfig()), "New renderer session failed");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::Vulkan,
        "A new renderer session did not probe Vulkan again");
    expect(vulkan->calls == 2, "New session did not recreate Vulkan");
}

void testRetryableRenderDoesNotChangeAPI()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    auto retryable = behavior({
        {
            false,
            qtav::VideoRenderEventType::RedrawRequested,
            {},
            false,
        },
    });
    vulkan->behaviors.push_back(retryable);

    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    expect(selector.open(renderConfig()), "Vulkan startup failed");
    expect(
        !selector.render(qtav::VideoFrame {}),
        "A retryable render attempt unexpectedly succeeded");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::Vulkan,
        "A retryable render attempt changed graphics APIs");
    expect(
        selector.presentationAvailable(),
        "A retryable render attempt retired the renderer");
    expect(openGLES->calls == 0, "Retryable render probed OpenGL ES");
}

void testDetailedRenderAttemptContract()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->behaviors.push_back(
        behavior({
            detailedAttempt(
                qtav::VideoRenderAttemptStatus::DeferredUntilRedraw,
                "waiting for a producer image"),
            detailedAttempt(
                qtav::VideoRenderAttemptStatus::RetryAfterBackoff,
                "in-flight ring is busy",
                7),
            detailedAttempt(
                qtav::VideoRenderAttemptStatus::Discarded,
                "stale native-image generation"),
            detailedAttempt(
                qtav::VideoRenderAttemptStatus::Presented),
        }));

    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    expect(selector.open(renderConfig()), "Vulkan startup failed");

    const auto deferred = selector.renderDetailed(qtav::VideoFrame {});
    expect(
        deferred.status
            == qtav::VideoRenderAttemptStatus::DeferredUntilRedraw,
        "Deferred-until-redraw status was not preserved");
    const auto retry = selector.renderDetailed(qtav::VideoFrame {});
    expect(
        retry.status
                == qtav::VideoRenderAttemptStatus::RetryAfterBackoff
            && retry.retryAfterMilliseconds == 7,
        "Retry-after-backoff status or delay was not preserved");
    const auto discarded = selector.renderDetailed(qtav::VideoFrame {});
    expect(
        discarded.status == qtav::VideoRenderAttemptStatus::Discarded
            && discarded.frameConsumed(),
        "Terminal discarded status was not preserved");
    const auto presented = selector.renderDetailed(qtav::VideoFrame {});
    expect(
        presented.status == qtav::VideoRenderAttemptStatus::Presented,
        "Presented status was not preserved");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::Vulkan
            && selector.presentationAvailable()
            && openGLES->calls == 0,
        "A nonfatal detailed status changed renderer selection");
}

void testHardwareFrameFallbackRoutes()
{
    {
        auto vulkan = std::make_shared<FactoryScript>();
        auto openGLES = std::make_shared<FactoryScript>();
        vulkan->behaviors.push_back(
            behavior({
                {
                    false,
                    qtav::VideoRenderEventType::Error,
                    "VK_ERROR_DEVICE_LOST",
                },
            }));
        auto openGLBehavior = behavior();
        openGLBehavior->hardwareDevices = {
            qtav::HardwareDeviceType::MediaCodec,
        };
        openGLES->behaviors.push_back(openGLBehavior);

        int mapCalls = 0;
        const qtav::VideoFrame oldFrame =
            hardwareFrame(0x1000U, 7, mapCalls);
        const qtav::VideoFrame newFrame =
            hardwareFrame(0x2000U, 8, mapCalls);
        qtav::MobileHardwareFrameFallbackEvent fallbackEvent;
        qtav::MobileVideoRendererSelector selector(
            selectorConfig(vulkan, openGLES));
        selector.setHardwareFrameFallbackCallback(
            [&fallbackEvent](
                const qtav::MobileHardwareFrameFallbackEvent& event) {
                fallbackEvent = event;
                return qtav::MobileHardwareFrameFallbackDecision {
                    qtav::MobileHardwareFrameFallbackRoute::
                        OpenGLESInterop,
                    "bound MediaCodec to the replacement native surface",
                };
            });
        expect(selector.open(renderConfig()), "Vulkan startup failed");
        expect(
            selector.render(oldFrame),
            "Hardware-frame fallback did not consume the retired frame");
        expect(
            selector.selectedAPI() == qtav::MobileRenderAPI::OpenGLES,
            "Hardware-frame fallback did not keep OpenGL ES active");
        expect(
            selector.hardwareFrameFallbackRoute()
                == qtav::MobileHardwareFrameFallbackRoute::
                    OpenGLESInterop,
            "OpenGL ES interop route was not recorded");
        expect(
            fallbackEvent.sourceDevice
                    == qtav::HardwareDeviceType::MediaCodec
                && fallbackEvent.sourceSurfaceGeneration == 7,
            "Hardware fallback omitted the retired source identity");
        expect(
            openGLBehavior->renderCount == 0,
            "The retired Vulkan hardware frame was retried through OpenGL ES");
        expect(
            selector.render(oldFrame)
                && openGLBehavior->renderCount == 0,
            "A late retired-surface frame reached OpenGL ES");
        expect(
            selector.render(newFrame)
                && openGLBehavior->renderCount == 1,
            "A replacement-surface hardware frame did not reach OpenGL ES");
        expect(mapCalls == 0, "Hardware fallback mapped a native frame");
    }

    {
        auto vulkan = std::make_shared<FactoryScript>();
        auto openGLES = std::make_shared<FactoryScript>();
        vulkan->behaviors.push_back(
            behavior({
                {
                    false,
                    qtav::VideoRenderEventType::Error,
                    "VK_ERROR_DEVICE_LOST",
                },
            }));
        auto openGLBehavior = behavior();
        openGLES->behaviors.push_back(openGLBehavior);
        int mapCalls = 0;
        const qtav::VideoFrame native =
            hardwareFrame(0x3000U, 9, mapCalls);

        qtav::MobileVideoRendererSelector selector(
            selectorConfig(vulkan, openGLES));
        selector.setHardwareFrameFallbackCallback(
            [](const qtav::MobileHardwareFrameFallbackEvent&) {
                return qtav::MobileHardwareFrameFallbackDecision {
                    qtav::MobileHardwareFrameFallbackRoute::
                        SoftwareDecode,
                    "reopened the video stream in software",
                };
            });
        expect(selector.open(renderConfig()), "Vulkan startup failed");
        expect(
            selector.render(native),
            "Software fallback did not consume the retired frame");
        expect(
            selector.render(native)
                && openGLBehavior->renderCount == 0,
            "Software fallback forwarded a late hardware frame");
        expect(
            selector.render(qtav::VideoFrame {})
                && openGLBehavior->renderCount == 1,
            "Software fallback did not render a later software frame");
        expect(mapCalls == 0, "Software-decode route mapped a native frame");
    }

    for (const auto route : {
             qtav::MobileHardwareFrameFallbackRoute::DirectSurface,
             qtav::MobileHardwareFrameFallbackRoute::NoVideo,
         }) {
        auto vulkan = std::make_shared<FactoryScript>();
        auto openGLES = std::make_shared<FactoryScript>();
        vulkan->behaviors.push_back(
            behavior({
                {
                    false,
                    qtav::VideoRenderEventType::Error,
                    "VK_ERROR_DEVICE_LOST",
                },
            }));
        auto openGLBehavior = behavior();
        if (route
            == qtav::MobileHardwareFrameFallbackRoute::DirectSurface) {
            openGLES->unavailableDetail =
                "No compatible OpenGL ES native interop";
        } else {
            openGLES->behaviors.push_back(openGLBehavior);
        }
        int mapCalls = 0;
        const qtav::VideoFrame native =
            hardwareFrame(0x4000U, 10, mapCalls);

        qtav::MobileVideoRendererSelector selector(
            selectorConfig(vulkan, openGLES));
        selector.setHardwareFrameFallbackCallback(
            [route](const qtav::MobileHardwareFrameFallbackEvent&) {
                return qtav::MobileHardwareFrameFallbackDecision {
                    route,
                    "caller-selected terminal renderer route",
                };
            });
        expect(selector.open(renderConfig()), "Vulkan startup failed");
        expect(
            selector.render(native),
            "Explicit terminal hardware route failed");
        expect(
            selector.selectedAPI() == qtav::MobileRenderAPI::None
                && !selector.presentationAvailable(),
            "Terminal hardware route left a renderer active");
        expect(
            selector.hardwareFrameFallbackRoute() == route,
            "Terminal hardware route was not recorded");
        expect(
            selector.render(native),
            "Terminal hardware route did not safely consume later frames");
        expect(
            openGLBehavior->renderCount == 0 && mapCalls == 0,
            "Terminal hardware route rendered or mapped a native frame");
    }
}

void testHardwareFrameFallbackCascadesToSoftwareDecode()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->behaviors.push_back(
        behavior({
            {
                false,
                qtav::VideoRenderEventType::Error,
                "VK_ERROR_FORMAT_NOT_SUPPORTED",
            },
        }));
    auto openGLBehavior = behavior({
        {
            false,
            qtav::VideoRenderEventType::Error,
            "OpenGL ES native-image import failed",
        },
    });
    openGLBehavior->hardwareDevices = {
        qtav::HardwareDeviceType::MediaCodec,
    };
    openGLES->behaviors.push_back(openGLBehavior);

    int callbackCalls = 0;
    int mapCalls = 0;
    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    selector.setHardwareFrameFallbackCallback(
        [&callbackCalls](
            const qtav::MobileHardwareFrameFallbackEvent& event) {
            ++callbackCalls;
            if (event.previousAPI == qtav::MobileRenderAPI::Vulkan) {
                return qtav::MobileHardwareFrameFallbackDecision {
                    qtav::MobileHardwareFrameFallbackRoute::
                        OpenGLESInterop,
                    "rebound decoder output to OpenGL ES",
                };
            }
            return qtav::MobileHardwareFrameFallbackDecision {
                qtav::MobileHardwareFrameFallbackRoute::SoftwareDecode,
                "disabled hardware decode after OpenGL ES interop failed",
            };
        });
    expect(selector.open(renderConfig()), "Vulkan startup failed");
    expect(
        selector.render(hardwareFrame(0x6000U, 12, mapCalls)),
        "Vulkan-to-OpenGL ES hardware fallback failed");
    expect(
        selector.render(hardwareFrame(0x7000U, 13, mapCalls)),
        "OpenGL ES hardware failure did not fall back to software decode");
    expect(
        selector.hardwareFrameFallbackRoute()
            == qtav::MobileHardwareFrameFallbackRoute::SoftwareDecode,
        "The final software-decode route was not recorded");
    expect(callbackCalls == 2, "The two-stage fallback callback was not run");
    expect(
        selector.render(qtav::VideoFrame {}),
        "A software frame was not rendered after the fallback chain");
    expect(mapCalls == 0, "The fallback chain mapped a hardware frame");
}

void testHardwareFrameFallbackRequiresExplicitPolicy()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->behaviors.push_back(
        behavior({
            {
                false,
                qtav::VideoRenderEventType::Error,
                "VK_ERROR_DEVICE_LOST",
            },
        }));
    auto openGLBehavior = behavior();
    openGLBehavior->hardwareDevices = {
        qtav::HardwareDeviceType::MediaCodec,
    };
    openGLES->behaviors.push_back(openGLBehavior);
    int mapCalls = 0;

    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    expect(selector.open(renderConfig()), "Vulkan startup failed");
    expect(
        !selector.render(hardwareFrame(0x5000U, 11, mapCalls)),
        "A hardware fallback without an explicit policy succeeded");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::None,
        "Missing hardware policy left OpenGL ES active");
    expect(
        selector.lastError().find("explicit") != std::string::npos,
        "Missing hardware policy did not report its requirement");
    expect(mapCalls == 0, "Missing hardware policy mapped a native frame");
}

void testRepeatedRecoveryFailureFallsBack()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->behaviors.push_back(
        behavior({
            {
                false,
                qtav::VideoRenderEventType::SurfaceLost,
                "scripted surface loss",
            },
        }));
    auto recoveryOne = behavior();
    recoveryOne->openSucceeded = false;
    auto recoveryTwo = behavior();
    recoveryTwo->openSucceeded = false;
    vulkan->behaviors.push_back(recoveryOne);
    vulkan->behaviors.push_back(recoveryTwo);
    openGLES->behaviors.push_back(behavior());

    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES, 2));
    expect(selector.open(renderConfig()), "Vulkan startup failed");
    expect(
        selector.render(qtav::VideoFrame {}),
        "Repeated recovery failure did not fall back");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::OpenGLES,
        "Repeated Vulkan recovery failure kept Vulkan active");
    expect(vulkan->calls == 3, "Recovery attempt bound was not honored");
    expect(openGLES->calls == 1, "OpenGL ES fallback was not created");
}

void testSurfaceSuspendAndSameAPIResume()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->behaviors.push_back(behavior());
    vulkan->behaviors.push_back(behavior());

    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    expect(selector.open(renderConfig()), "Vulkan startup failed");
    selector.suspendSurface();
    expect(
        !selector.presentationAvailable(),
        "Suspended surface remained available");
    expect(
        selector.recreateSurface(),
        "Replacement surface did not recreate Vulkan");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::Vulkan,
        "Replacement surface changed graphics API");
    expect(openGLES->calls == 0, "Replacement surface probed OpenGL ES");
}

void testRecoverableOpenGLESRecreation()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->unavailableDetail = "Vulkan unavailable";
    openGLES->behaviors.push_back(
        behavior({
            {
                false,
                qtav::VideoRenderEventType::SurfaceLost,
                "EGL_CONTEXT_LOST",
            },
        }));
    openGLES->behaviors.push_back(behavior());

    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    expect(selector.open(renderConfig()), "OpenGL ES startup failed");
    expect(
        selector.render(qtav::VideoFrame {}),
        "Recoverable OpenGL ES recreation failed");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::OpenGLES,
        "OpenGL ES recovery changed graphics API");
    expect(openGLES->calls == 2, "OpenGL ES was not recreated");
    expect(vulkan->calls == 1, "Vulkan was reprobed after fallback");
}

void testBothBackendsUnavailable()
{
    auto vulkan = std::make_shared<FactoryScript>();
    auto openGLES = std::make_shared<FactoryScript>();
    vulkan->unavailableDetail = "No Vulkan device";
    openGLES->unavailableDetail = "No EGLConfig";

    int errorEvents = 0;
    qtav::MobileVideoRendererSelector selector(
        selectorConfig(vulkan, openGLES));
    selector.setEventCallback(
        [&errorEvents](const qtav::VideoRenderEvent& event) {
            if (event.type == qtav::VideoRenderEventType::Error) {
                ++errorEvents;
            }
        });
    expect(
        !selector.open(renderConfig()),
        "Unavailable backends unexpectedly opened");
    expect(
        selector.selectedAPI() == qtav::MobileRenderAPI::None,
        "Unavailable backends left an API selected");
    expect(
        !selector.presentationAvailable(),
        "Unavailable backends reported presentation");
    expect(errorEvents == 1, "Unavailable event was not emitted once");
    expect(
        selector.lastError().find("No Vulkan device")
            != std::string::npos
            && selector.lastError().find("No EGLConfig")
                != std::string::npos,
        "Unavailable diagnostics omitted a backend reason");
}

} // namespace

int main()
{
    testPreferredVulkan();
    testPreferredOpenGLES();
    testVulkanUnavailableAndInitialFailure();
    testRecoverableVulkanRecreation();
    testFatalOneWayFallback();
    testRetryableRenderDoesNotChangeAPI();
    testDetailedRenderAttemptContract();
    testHardwareFrameFallbackRoutes();
    testHardwareFrameFallbackCascadesToSoftwareDecode();
    testHardwareFrameFallbackRequiresExplicitPolicy();
    testRepeatedRecoveryFailureFallsBack();
    testSurfaceSuspendAndSameAPIResume();
    testRecoverableOpenGLESRecreation();
    testBothBackendsUnavailable();
    return 0;
}
