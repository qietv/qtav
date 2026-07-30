// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_activity.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char* LogTag = "QtAVCoreTest";
constexpr const char* AssetName = "qtav-test.avi";

void logInfo(const std::string& message)
{
    __android_log_print(ANDROID_LOG_INFO, LogTag, "%s", message.c_str());
}

void logError(const std::string& message)
{
    __android_log_print(ANDROID_LOG_ERROR, LogTag, "%s", message.c_str());
}

bool writeAll(int descriptor, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        const ssize_t written = write(descriptor, bytes, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

std::string copyTestAsset(ANativeActivity* activity)
{
    if (!activity || !activity->assetManager || !activity->internalDataPath) {
        return {};
    }

    AAsset* asset = AAssetManager_open(
        activity->assetManager,
        AssetName,
        AASSET_MODE_STREAMING);
    if (!asset) {
        return {};
    }

    const std::string destination =
        std::string(activity->internalDataPath) + '/' + AssetName;
    const int descriptor = open(
        destination.c_str(),
        O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        AAsset_close(asset);
        return {};
    }

    bool succeeded = true;
    std::uint8_t buffer[16 * 1024];
    for (;;) {
        const int count = AAsset_read(asset, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0
            || !writeAll(
                descriptor,
                buffer,
                static_cast<std::size_t>(count))) {
            succeeded = false;
            break;
        }
    }

    if (close(descriptor) != 0) {
        succeeded = false;
    }
    AAsset_close(asset);
    if (!succeeded) {
        unlink(destination.c_str());
        return {};
    }
    return destination;
}

struct TestState {
    explicit TestState(ANativeActivity* nativeActivity)
        : activity(nativeActivity)
    {
    }

    void fail(const std::string& detail)
    {
        bool expected = false;
        if (finished.compare_exchange_strong(expected, true)) {
            logError("QTAV_ANDROID_TEST: FAIL " + detail);
        }
    }

    void pass()
    {
        bool expected = false;
        if (!finished.compare_exchange_strong(expected, true)) {
            return;
        }
        logInfo(
            "QTAV_ANDROID_TEST: PASS video_frames="
            + std::to_string(videoFrames.load())
            + " audio_frames=" + std::to_string(audioFrames.load()));
    }

    void start()
    {
        mediaPath = copyTestAsset(activity);
        if (mediaPath.empty()) {
            fail("could not copy packaged media asset");
            return;
        }

        player
            .onEvent([](const qtav::MediaEvent& event) {
                logError(
                    "event category=" + event.category
                    + " error=" + std::to_string(event.error)
                    + " detail=" + event.detail);
                return false;
            })
            .onMediaStatus(
                [this](qtav::MediaStatus, qtav::MediaStatus status) {
                    if (status == qtav::MediaStatus::Invalid) {
                        fail("media status became invalid");
                    } else if (status == qtav::MediaStatus::EndOfMedia) {
                        if (videoFrames.load() < 10) {
                            fail("too few decoded video frames");
                        } else if (audioFrames.load() == 0) {
                            fail("no decoded audio frames");
                        } else {
                            pass();
                        }
                    }
                    return false;
                })
            .onVideoFrame([this](const qtav::VideoFrame& frame, int) {
                if (!frame || frame.width() != 160 || frame.height() != 90) {
                    fail("unexpected video frame");
                    return;
                }
                ++videoFrames;
            })
            .onAudioFrame([this](const qtav::AudioFrame& frame, int) {
                if (!frame || frame.sampleRate() != 48'000) {
                    fail("unexpected audio frame");
                    return;
                }
                ++audioFrames;
            });

        logInfo("QTAV_ANDROID_TEST: START media=" + mediaPath);
        player.setPlaybackRate(4.0F);
        player.setMedia(mediaPath);
        player.setState(qtav::State::Playing);
    }

    ANativeActivity* activity = nullptr;
    qtav::Player player;
    std::string mediaPath;
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> audioFrames { 0 };
    std::atomic<bool> finished { false };
};

void onDestroy(ANativeActivity* activity)
{
    if (!activity) {
        return;
    }
    auto* state = static_cast<TestState*>(activity->instance);
    activity->instance = nullptr;
    delete state;
}

} // namespace

extern "C" __attribute__((visibility("default")))
void ANativeActivity_onCreate(
    ANativeActivity* activity,
    void*,
    std::size_t)
{
    if (!activity || !activity->callbacks) {
        return;
    }
    auto state = std::make_unique<TestState>(activity);
    activity->callbacks->onDestroy = onDestroy;
    activity->instance = state.release();
    static_cast<TestState*>(activity->instance)->start();
}
