// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mobile_video_renderer.h>

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
};

struct RendererBehavior {
    bool openSucceeded = true;
    bool configureSucceeded = true;
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

    bool render(const qtav::VideoFrame&) override
    {
        ++behavior_->renderCount;
        RenderResult result;
        if (!behavior_->renderResults.empty()) {
            result = std::move(behavior_->renderResults.front());
            behavior_->renderResults.pop_front();
        }
        if (!result.succeeded && callback_) {
            callback_({ result.eventType, std::move(result.detail) });
        }
        return result.succeeded;
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
            {
                false,
                qtav::VideoRenderEventType::SurfaceLost,
                "scripted recoverable surface loss",
            },
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
            {
                false,
                qtav::VideoRenderEventType::Error,
                "VK_ERROR_DEVICE_LOST",
            },
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
    testVulkanUnavailableAndInitialFailure();
    testRecoverableVulkanRecreation();
    testFatalOneWayFallback();
    testRepeatedRecoveryFailureFallsBack();
    testSurfaceSuspendAndSameAPIResume();
    testRecoverableOpenGLESRecreation();
    testBothBackendsUnavailable();
    return 0;
}
