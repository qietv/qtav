// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/wasapi_audio_sink.h>

#include <audioclient.h>
#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace qtav {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kMinimumBufferMilliseconds = 20;
constexpr int kMaximumBufferMilliseconds = 2'000;
constexpr int kMinimumQueuedMilliseconds = 100;
constexpr int kMaximumQueuedMilliseconds = 5'000;
constexpr DWORD kSinkWaitMilliseconds = 2'000;

bool sameFormat(const AudioFormat& left, const AudioFormat& right)
{
    return left.sampleRate == right.sampleRate
        && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat
        && left.channelLayout == right.channelLayout;
}

std::string defaultChannelLayout(int channels)
{
    if (channels == 1) {
        return "mono";
    }
    if (channels == 2) {
        return "stereo";
    }
    return {};
}

std::int64_t millisecondsForFrames(
    std::uint64_t frames,
    int sampleRate) noexcept
{
    if (sampleRate <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(
        (frames * 1'000ULL
         + static_cast<std::uint64_t>(sampleRate / 2))
        / static_cast<std::uint64_t>(sampleRate));
}

std::string hresultText(HRESULT result)
{
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << std::uppercase
           << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(result);
    return stream.str();
}

bool isDeviceLost(HRESULT result) noexcept
{
    return result == AUDCLNT_E_DEVICE_INVALIDATED
        || result == AUDCLNT_E_RESOURCES_INVALIDATED
        || result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

WAVEFORMATEXTENSIBLE floatFormat(
    int sampleRate,
    int channels) noexcept
{
    WAVEFORMATEXTENSIBLE result {};
    result.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    result.Format.nChannels = static_cast<WORD>(channels);
    result.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    result.Format.wBitsPerSample = 32;
    result.Format.nBlockAlign =
        static_cast<WORD>(channels * static_cast<int>(sizeof(float)));
    result.Format.nAvgBytesPerSec =
        result.Format.nSamplesPerSec * result.Format.nBlockAlign;
    result.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE)
        - sizeof(WAVEFORMATEX);
    result.Samples.wValidBitsPerSample = 32;
    result.dwChannelMask = channels == 1
        ? SPEAKER_FRONT_CENTER
        : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    result.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return result;
}

AudioFormat audioFormat(const WAVEFORMATEX& format)
{
    SampleFormat sampleFormat = SampleFormat::Unknown;
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT
        && format.wBitsPerSample == 32) {
        sampleFormat = SampleFormat::Float;
    } else if (format.wFormatTag == WAVE_FORMAT_PCM) {
        if (format.wBitsPerSample == 8) {
            sampleFormat = SampleFormat::U8;
        } else if (format.wBitsPerSample == 16) {
            sampleFormat = SampleFormat::S16;
        } else if (format.wBitsPerSample == 32) {
            sampleFormat = SampleFormat::S32;
        }
    } else if (
        format.wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format.cbSize
            >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (IsEqualGUID(
                extensible.SubFormat,
                KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            && format.wBitsPerSample == 32) {
            sampleFormat = SampleFormat::Float;
        } else if (
            IsEqualGUID(
                extensible.SubFormat,
                KSDATAFORMAT_SUBTYPE_PCM)) {
            if (format.wBitsPerSample == 8) {
                sampleFormat = SampleFormat::U8;
            } else if (format.wBitsPerSample == 16) {
                sampleFormat = SampleFormat::S16;
            } else if (format.wBitsPerSample == 32) {
                sampleFormat = SampleFormat::S32;
            }
        }
    }
    return {
        static_cast<int>(format.nSamplesPerSec),
        static_cast<int>(format.nChannels),
        sampleFormat,
        defaultChannelLayout(static_cast<int>(format.nChannels)),
    };
}

} // namespace

class WasapiAudioSink::Impl {
public:
    struct QueuedBuffer {
        std::vector<std::uint8_t> data;
        int frames = 0;
        int offset = 0;
        std::int64_t timestamp = 0;
    };

    explicit Impl(WasapiAudioSinkConfig value)
        : config_(std::move(value))
    {
        config_.maximumChannels =
            std::clamp(config_.maximumChannels, 1, 2);
        config_.bufferMilliseconds = std::clamp(
            config_.bufferMilliseconds,
            kMinimumBufferMilliseconds,
            kMaximumBufferMilliseconds);
        config_.maximumQueuedMilliseconds = std::clamp(
            config_.maximumQueuedMilliseconds,
            kMinimumQueuedMilliseconds,
            kMaximumQueuedMilliseconds);
    }

    ~Impl()
    {
        shutdown();
    }

    void shutdown() noexcept
    {
        std::thread worker;
        HANDLE wake = nullptr;
        HANDLE render = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
            wake = wakeEvent_;
            worker = std::move(thread_);
        }
        if (wake) {
            SetEvent(wake);
        }
        changed_.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wake = wakeEvent_;
            render = renderEvent_;
            wakeEvent_ = nullptr;
            renderEvent_ = nullptr;
            queue_.clear();
            queuedFrames_ = 0;
            open_ = false;
            initialized_ = false;
            started_ = false;
            paused_ = false;
            pauseRequested_ = false;
            stopRequested_ = false;
            fatal_ = false;
            flushRequested_ = 0;
            flushCompleted_ = 0;
            drainRequested_ = 0;
            drainCompleted_ = 0;
            clock_ = {};
            hasTimelineAnchor_ = false;
            underrunReported_ = false;
            emptySinceValid_ = false;
            deviceFormat_ = {};
            activeEndpoint_ = {};
        }
        if (render) {
            CloseHandle(render);
        }
        if (wake) {
            CloseHandle(wake);
        }
        changed_.notify_all();
    }

    void publishRuntimeFailure(
        HRESULT result,
        const char* operation)
    {
        EventCallback callback;
        AudioSinkEvent event;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fatal_ = true;
            open_ = false;
            callback = callback_;
            event = {
                isDeviceLost(result)
                    ? AudioSinkEventType::DeviceLost
                    : AudioSinkEventType::Error,
                std::string(operation) + ": " + hresultText(result),
            };
        }
        changed_.notify_all();
        if (wakeEvent_) {
            SetEvent(wakeEvent_);
        }
        if (callback) {
            callback(event);
        }
    }

    void publishUnderrun()
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (callback) {
            callback({
                AudioSinkEventType::Underrun,
                "The WASAPI shared-mode buffer underrun",
            });
        }
    }

    bool initializeClient(
        const AudioFormat& decodedFormat,
        ComPtr<IAudioClient>& client,
        ComPtr<IAudioRenderClient>& renderClient,
        ComPtr<IAudioClock>& audioClock,
        UINT32& engineBufferFrames,
        UINT64& clockFrequency,
        REFERENCE_TIME& streamLatency,
        std::string& error)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) {
            error = "Could not create the WASAPI device enumerator: "
                + hresultText(result);
            return false;
        }

        WasapiEndpointId configuredEndpoint;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            configuredEndpoint = config_.endpoint;
        }
        ComPtr<IMMDevice> device;
        if (configuredEndpoint) {
            result = enumerator->GetDevice(
                configuredEndpoint.value().c_str(),
                &device);
        } else {
            result = enumerator->GetDefaultAudioEndpoint(
                eRender,
                eMultimedia,
                &device);
        }
        if (FAILED(result) || !device) {
            error = "No WASAPI output endpoint is available: "
                + hresultText(result);
            return false;
        }

        LPWSTR endpointValue = nullptr;
        result = device->GetId(&endpointValue);
        if (FAILED(result) || !endpointValue) {
            error = "Could not query the WASAPI endpoint identifier: "
                + hresultText(result);
            return false;
        }
        WasapiEndpointId activeEndpoint {
            std::wstring(endpointValue),
        };
        CoTaskMemFree(endpointValue);

        result = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            &client);
        if (FAILED(result) || !client) {
            error = "Could not activate the WASAPI audio client: "
                + hresultText(result);
            return false;
        }

        WAVEFORMATEX* mixFormat = nullptr;
        result = client->GetMixFormat(&mixFormat);
        if (FAILED(result) || !mixFormat) {
            error = "Could not query the WASAPI mix format: "
                + hresultText(result);
            return false;
        }
        const int sampleRate =
            static_cast<int>(mixFormat->nSamplesPerSec);
        const int channels = std::min(
            decodedFormat.channels,
            std::min(
                static_cast<int>(mixFormat->nChannels),
                config_.maximumChannels));
        CoTaskMemFree(mixFormat);
        if (sampleRate <= 0 || channels <= 0) {
            error = "The WASAPI endpoint has no usable PCM format";
            return false;
        }

        auto requested = floatFormat(sampleRate, channels);
        WAVEFORMATEX* closest = nullptr;
        result = client->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED,
            &requested.Format,
            &closest);
        if (closest) {
            CoTaskMemFree(closest);
        }
        if (FAILED(result)) {
            error =
                "The WASAPI endpoint does not accept interleaved Float32 "
                "PCM in shared mode: "
                + hresultText(result);
            return false;
        }

        const auto requestedDuration =
            static_cast<REFERENCE_TIME>(
                config_.bufferMilliseconds)
            * 10'000;
        constexpr DWORD flags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK
            | AUDCLNT_STREAMFLAGS_NOPERSIST
            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        result = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            flags,
            requestedDuration,
            0,
            &requested.Format,
            nullptr);
        if (FAILED(result)) {
            error =
                "Could not initialize the WASAPI shared-mode stream: "
                + hresultText(result);
            return false;
        }
        result = client->SetEventHandle(renderEvent_);
        if (FAILED(result)) {
            error = "Could not set the WASAPI render event: "
                + hresultText(result);
            return false;
        }
        result = client->GetBufferSize(&engineBufferFrames);
        if (FAILED(result) || engineBufferFrames == 0) {
            error = "Could not query the WASAPI buffer size: "
                + hresultText(result);
            return false;
        }
        result = client->GetStreamLatency(&streamLatency);
        if (FAILED(result)) {
            streamLatency = 0;
        }
        result = client->GetService(IID_PPV_ARGS(&renderClient));
        if (FAILED(result) || !renderClient) {
            error = "Could not acquire the WASAPI render client: "
                + hresultText(result);
            return false;
        }
        result = client->GetService(IID_PPV_ARGS(&audioClock));
        if (FAILED(result) || !audioClock) {
            error = "Could not acquire the WASAPI audio clock: "
                + hresultText(result);
            return false;
        }
        result = audioClock->GetFrequency(&clockFrequency);
        if (FAILED(result) || clockFrequency == 0) {
            error = "Could not query the WASAPI audio clock frequency: "
                + hresultText(result);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            deviceFormat_ = audioFormat(requested.Format);
            activeEndpoint_ = std::move(activeEndpoint);
            engineBufferFrames_ = engineBufferFrames;
            streamLatencyMilliseconds_ = std::max<std::int64_t>(
                0,
                (streamLatency + 5'000) / 10'000);
        }
        return true;
    }

    void updateClock(
        IAudioClient* client,
        IAudioClock* audioClock,
        UINT64 frequency)
    {
        UINT32 padding = 0;
        const HRESULT paddingResult =
            client->GetCurrentPadding(&padding);
        if (FAILED(paddingResult)) {
            publishRuntimeFailure(
                paddingResult,
                "Could not query WASAPI buffer padding");
            return;
        }

        UINT64 position = 0;
        UINT64 qpc = 0;
        const HRESULT clockResult =
            audioClock->GetPosition(&position, &qpc);
        std::lock_guard<std::mutex> lock(mutex_);
        clock_.latencyMilliseconds =
            millisecondsForFrames(
                static_cast<std::uint64_t>(padding),
                deviceFormat_.sampleRate)
            + streamLatencyMilliseconds_;
        if (SUCCEEDED(clockResult) && hasTimelineAnchor_
            && position >= timelineAnchorDevicePosition_) {
            clock_.valid = true;
            clock_.positionMilliseconds =
                timelineAnchorTimestamp_
                + millisecondsForFrames(
                    position - timelineAnchorDevicePosition_,
                    static_cast<int>(frequency));
        } else {
            clock_.valid = false;
        }
    }

    bool fill(
        IAudioClient* client,
        IAudioRenderClient* renderClient,
        IAudioClock* audioClock,
        UINT64 clockFrequency)
    {
        UINT32 padding = 0;
        HRESULT result = client->GetCurrentPadding(&padding);
        if (FAILED(result)) {
            publishRuntimeFailure(
                result,
                "Could not query WASAPI buffer padding");
            return false;
        }

        UINT32 framesToWrite = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto available = engineBufferFrames_ > padding
                ? engineBufferFrames_ - padding
                : 0;
            framesToWrite = static_cast<UINT32>(
                std::min<std::int64_t>(
                    available,
                    queuedFrames_));
        }
        if (framesToWrite == 0) {
            bool underrun = false;
            bool drainComplete = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                drainComplete = drainRequested_ > drainCompleted_
                    && queuedFrames_ == 0 && padding == 0;
                const bool emptyPlayback = started_ && !paused_
                    && drainRequested_ == drainCompleted_
                    && queuedFrames_ == 0 && padding == 0;
                if (emptyPlayback) {
                    hasTimelineAnchor_ = false;
                    clock_.valid = false;
                    const auto now =
                        std::chrono::steady_clock::now();
                    if (!emptySinceValid_) {
                        emptySince_ = now;
                        emptySinceValid_ = true;
                    }
                    underrun =
                        now - emptySince_
                            >= std::chrono::milliseconds(100)
                        && !underrunReported_;
                    underrunReported_ =
                        underrunReported_ || underrun;
                }
            }
            if (drainComplete) {
                client->Stop();
                client->Reset();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    started_ = false;
                    hasTimelineAnchor_ = false;
                    emptySinceValid_ = false;
                    underrunReported_ = false;
                    drainCompleted_ = drainRequested_;
                }
                changed_.notify_all();
            }
            if (underrun) {
                publishUnderrun();
            }
            updateClock(client, audioClock, clockFrequency);
            return true;
        }

        BYTE* destination = nullptr;
        result = renderClient->GetBuffer(
            framesToWrite,
            &destination);
        if (FAILED(result) || !destination) {
            publishRuntimeFailure(
                result,
                "Could not acquire the WASAPI render buffer");
            return false;
        }

        const int bytesPerFrame = static_cast<int>(sizeof(float))
            * deviceFormat_.channels;
        UINT32 copiedFrames = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (copiedFrames < framesToWrite
                   && !queue_.empty()) {
                auto& buffer = queue_.front();
                const int available =
                    buffer.frames - buffer.offset;
                const int frames = std::min<int>(
                    available,
                    framesToWrite - copiedFrames);
                if (!hasTimelineAnchor_) {
                    UINT64 devicePosition = 0;
                    UINT64 qpc = 0;
                    if (SUCCEEDED(audioClock->GetPosition(
                            &devicePosition,
                            &qpc))) {
                        timelineAnchorTimestamp_ =
                            buffer.timestamp
                            + millisecondsForFrames(
                                static_cast<std::uint64_t>(
                                    buffer.offset),
                                deviceFormat_.sampleRate);
                        timelineAnchorDevicePosition_ =
                            devicePosition;
                        hasTimelineAnchor_ = true;
                    }
                }
                const auto sourceOffset =
                    static_cast<std::size_t>(buffer.offset)
                    * static_cast<std::size_t>(bytesPerFrame);
                const auto destinationOffset =
                    static_cast<std::size_t>(copiedFrames)
                    * static_cast<std::size_t>(bytesPerFrame);
                const auto bytes =
                    static_cast<std::size_t>(frames)
                    * static_cast<std::size_t>(bytesPerFrame);
                std::memcpy(
                    destination + destinationOffset,
                    buffer.data.data() + sourceOffset,
                    bytes);
                buffer.offset += frames;
                copiedFrames += static_cast<UINT32>(frames);
                queuedFrames_ -= frames;
                if (buffer.offset == buffer.frames) {
                    queue_.pop_front();
                }
            }
            emptySinceValid_ = false;
            underrunReported_ = false;
        }
        changed_.notify_all();

        result = renderClient->ReleaseBuffer(copiedFrames, 0);
        if (FAILED(result)) {
            publishRuntimeFailure(
                result,
                "Could not submit the WASAPI render buffer");
            return false;
        }

        bool shouldStart = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shouldStart = !started_ && !paused_;
        }
        if (shouldStart) {
            result = client->Start();
            if (FAILED(result)) {
                publishRuntimeFailure(
                    result,
                    "Could not start the WASAPI stream");
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            started_ = true;
        }
        updateClock(client, audioClock, clockFrequency);
        return true;
    }

    bool processControls(IAudioClient* client)
    {
        bool stop = false;
        bool requestedPause = false;
        bool pauseChanged = false;
        std::uint64_t flushSerial = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop = stopRequested_ || fatal_;
            requestedPause = pauseRequested_;
            pauseChanged = requestedPause != paused_;
            if (flushRequested_ > flushCompleted_) {
                flushSerial = flushRequested_;
            }
        }
        if (stop) {
            return false;
        }
        if (flushSerial != 0) {
            HRESULT result = client->Stop();
            if (SUCCEEDED(result)) {
                result = client->Reset();
            }
            if (FAILED(result)) {
                publishRuntimeFailure(
                    result,
                    "Could not flush the WASAPI stream");
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.clear();
                queuedFrames_ = 0;
                started_ = false;
                hasTimelineAnchor_ = false;
                emptySinceValid_ = false;
                underrunReported_ = false;
                clock_ = {};
                drainCompleted_ = drainRequested_;
                flushCompleted_ = flushSerial;
            }
            changed_.notify_all();
        }
        if (pauseChanged) {
            bool started = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                started = started_;
            }
            HRESULT result = S_OK;
            if (started) {
                result = requestedPause
                    ? client->Stop()
                    : client->Start();
            }
            if (FAILED(result)) {
                publishRuntimeFailure(
                    result,
                    "Could not change the WASAPI pause state");
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                paused_ = requestedPause;
            }
            changed_.notify_all();
        }
        return true;
    }

    void threadMain(AudioFormat decodedFormat)
    {
        const HRESULT comResult = CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED);
        const bool uninitializeCom =
            comResult == S_OK || comResult == S_FALSE;
        if (FAILED(comResult)) {
            std::lock_guard<std::mutex> lock(mutex_);
            initializationResult_ = {
                false,
                {},
                "Could not initialize COM for WASAPI: "
                    + hresultText(comResult),
            };
            initialized_ = true;
            changed_.notify_all();
            return;
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(
            L"Pro Audio",
            &taskIndex);
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> renderClient;
        ComPtr<IAudioClock> audioClock;
        UINT32 engineBufferFrames = 0;
        UINT64 clockFrequency = 0;
        REFERENCE_TIME streamLatency = 0;
        std::string error;
        const bool ready = initializeClient(
            decodedFormat,
            client,
            renderClient,
            audioClock,
            engineBufferFrames,
            clockFrequency,
            streamLatency,
            error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            initializationResult_ = {
                ready,
                ready ? deviceFormat_ : AudioFormat {},
                std::move(error),
            };
            initialized_ = true;
            open_ = ready;
        }
        changed_.notify_all();

        if (ready) {
            HANDLE events[] { wakeEvent_, renderEvent_ };
            bool running = true;
            while (running) {
                const DWORD waitResult = WaitForMultipleObjects(
                    2,
                    events,
                    FALSE,
                    INFINITE);
                if (waitResult != WAIT_OBJECT_0
                    && waitResult != WAIT_OBJECT_0 + 1) {
                    publishRuntimeFailure(
                        HRESULT_FROM_WIN32(GetLastError()),
                        "Could not wait for the WASAPI render event");
                    break;
                }
                running = processControls(client.Get());
                if (running) {
                    running = fill(
                        client.Get(),
                        renderClient.Get(),
                        audioClock.Get(),
                        clockFrequency);
                }
            }
            client->Stop();
            client->Reset();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
        }
        changed_.notify_all();
        audioClock.Reset();
        renderClient.Reset();
        client.Reset();
        if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        if (uninitializeCom) {
            CoUninitialize();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    WasapiAudioSinkConfig config_;
    EventCallback callback_;
    std::thread thread_;
    HANDLE wakeEvent_ = nullptr;
    HANDLE renderEvent_ = nullptr;
    std::deque<QueuedBuffer> queue_;
    std::int64_t queuedFrames_ = 0;
    AudioSinkOpenResult initializationResult_;
    AudioFormat deviceFormat_;
    WasapiEndpointId activeEndpoint_;
    AudioSinkClock clock_;
    UINT32 engineBufferFrames_ = 0;
    std::int64_t streamLatencyMilliseconds_ = 0;
    std::int64_t timelineAnchorTimestamp_ = 0;
    UINT64 timelineAnchorDevicePosition_ = 0;
    std::uint64_t flushRequested_ = 0;
    std::uint64_t flushCompleted_ = 0;
    std::uint64_t drainRequested_ = 0;
    std::uint64_t drainCompleted_ = 0;
    bool initialized_ = false;
    bool open_ = false;
    bool started_ = false;
    bool paused_ = false;
    bool pauseRequested_ = false;
    bool stopRequested_ = false;
    bool fatal_ = false;
    bool hasTimelineAnchor_ = false;
    bool underrunReported_ = false;
    bool emptySinceValid_ = false;
    std::chrono::steady_clock::time_point emptySince_;
};

WasapiEndpointId::WasapiEndpointId(std::wstring value)
    : value_(std::move(value))
{
}

const std::wstring& WasapiEndpointId::value() const noexcept
{
    return value_;
}

WasapiEndpointId::operator bool() const noexcept
{
    return !value_.empty();
}

WasapiAudioSink::WasapiAudioSink(WasapiAudioSinkConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

WasapiAudioSink::~WasapiAudioSink() = default;
WasapiAudioSink::WasapiAudioSink(WasapiAudioSink&&) noexcept = default;
WasapiAudioSink& WasapiAudioSink::operator=(
    WasapiAudioSink&&) noexcept = default;

AudioSinkCapabilities WasapiAudioSink::capabilities() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return {
        { SampleFormat::Float },
        8'000,
        384'000,
        impl_->config_.maximumChannels,
        true,
        true,
    };
}

void WasapiAudioSink::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->callback_ = std::move(callback);
}

AudioSinkOpenResult WasapiAudioSink::open(
    const AudioFormat& decodedFormat)
{
    if (!impl_) {
        return {
            false,
            {},
            "The WASAPI audio sink has been moved from",
        };
    }
    if (!decodedFormat.isValid()) {
        return {
            false,
            {},
            "The decoded audio format is invalid",
        };
    }

    impl_->shutdown();
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE render = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake || !render) {
        if (render) {
            CloseHandle(render);
        }
        if (wake) {
            CloseHandle(wake);
        }
        return {
            false,
            {},
            "Could not create WASAPI synchronization events",
        };
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->wakeEvent_ = wake;
        impl_->renderEvent_ = render;
        impl_->initializationResult_ = {};
        impl_->initialized_ = false;
        impl_->thread_ = std::thread(
            &Impl::threadMain,
            impl_.get(),
            decodedFormat);
    }

    AudioSinkOpenResult result;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex_);
        impl_->changed_.wait(lock, [this] {
            return impl_->initialized_;
        });
        result = impl_->initializationResult_;
    }
    if (!result.success) {
        impl_->shutdown();
    }
    return result;
}

void WasapiAudioSink::close() noexcept
{
    if (impl_) {
        impl_->shutdown();
    }
}

void WasapiAudioSink::pause(bool paused)
{
    if (!impl_) {
        return;
    }
    HANDLE wake = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (!impl_->open_ || impl_->fatal_) {
            return;
        }
        impl_->pauseRequested_ = paused;
        wake = impl_->wakeEvent_;
    }
    SetEvent(wake);
    std::unique_lock<std::mutex> lock(impl_->mutex_);
    impl_->changed_.wait_for(
        lock,
        std::chrono::milliseconds(kSinkWaitMilliseconds),
        [this, paused] {
            return !impl_->open_ || impl_->fatal_
                || impl_->paused_ == paused;
        });
}

void WasapiAudioSink::flush()
{
    if (!impl_) {
        return;
    }
    HANDLE wake = nullptr;
    std::uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (!impl_->open_ || impl_->fatal_) {
            return;
        }
        serial = ++impl_->flushRequested_;
        wake = impl_->wakeEvent_;
    }
    SetEvent(wake);
    std::unique_lock<std::mutex> lock(impl_->mutex_);
    impl_->changed_.wait_for(
        lock,
        std::chrono::milliseconds(kSinkWaitMilliseconds),
        [this, serial] {
            return !impl_->open_ || impl_->fatal_
                || impl_->flushCompleted_ >= serial;
        });
}

bool WasapiAudioSink::write(const AudioBufferView& buffer)
{
    if (!impl_ || !buffer.isValid()
        || buffer.planes.size() != 1) {
        return false;
    }

    HANDLE wake = nullptr;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex_);
        if (!impl_->open_ || impl_->fatal_
            || impl_->pauseRequested_ || impl_->paused_
            || !sameFormat(buffer.format, impl_->deviceFormat_)) {
            return false;
        }
        const auto bytes = static_cast<std::uint64_t>(
                               buffer.samplesPerChannel)
            * static_cast<std::uint64_t>(buffer.format.channels)
            * sizeof(float);
        if (bytes == 0
            || bytes > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())
            || bytes > static_cast<std::uint64_t>(
                buffer.lineSizes.front())) {
            return false;
        }
        const auto maximumFrames = std::max<std::int64_t>(
            1,
            static_cast<std::int64_t>(
                impl_->deviceFormat_.sampleRate)
                * impl_->config_.maximumQueuedMilliseconds
                / 1'000);
        const bool available = impl_->changed_.wait_for(
            lock,
            std::chrono::milliseconds(kSinkWaitMilliseconds),
            [this, maximumFrames, &buffer] {
                return !impl_->open_ || impl_->fatal_
                    || impl_->pauseRequested_ || impl_->paused_
                    || impl_->queuedFrames_ == 0
                    || impl_->queuedFrames_
                            + buffer.samplesPerChannel
                        <= maximumFrames;
            });
        if (!available || !impl_->open_ || impl_->fatal_
            || impl_->pauseRequested_ || impl_->paused_) {
            return false;
        }
        Impl::QueuedBuffer queued;
        queued.data.resize(static_cast<std::size_t>(bytes));
        std::memcpy(
            queued.data.data(),
            buffer.planes.front(),
            queued.data.size());
        queued.frames = buffer.samplesPerChannel;
        queued.timestamp = buffer.timestamp;
        impl_->queuedFrames_ += queued.frames;
        impl_->queue_.push_back(std::move(queued));
        wake = impl_->wakeEvent_;
    }
    SetEvent(wake);
    return true;
}

bool WasapiAudioSink::drain()
{
    if (!impl_) {
        return false;
    }
    HANDLE wake = nullptr;
    std::uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (!impl_->open_ || impl_->fatal_
            || impl_->pauseRequested_ || impl_->paused_) {
            return false;
        }
        serial = ++impl_->drainRequested_;
        wake = impl_->wakeEvent_;
    }
    SetEvent(wake);
    std::unique_lock<std::mutex> lock(impl_->mutex_);
    return impl_->changed_.wait_for(
        lock,
        std::chrono::seconds(10),
        [this, serial] {
            return !impl_->open_ || impl_->fatal_
                || impl_->drainCompleted_ >= serial;
        })
        && !impl_->fatal_
        && impl_->drainCompleted_ >= serial;
}

AudioSinkClock WasapiAudioSink::clock() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->clock_;
}

WasapiEndpointId WasapiAudioSink::endpoint() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->activeEndpoint_;
}

AudioFormat WasapiAudioSink::deviceFormat() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->deviceFormat_;
}

} // namespace qtav
