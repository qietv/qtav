# OHOS deferred-render seek investigation and repair

Date: 2026-08-12

## Symptom and evidence

The user-facing OHOS player could continue audio and media-position progress
while its picture remained frozen after seeking forward and then backward. A
captured `legend.mkv` session advanced from roughly 2:52 to 3:25 and continued
receiving OHCodec video outputs, while successful presentation FPS stayed at
zero. Only the first few private-consumer callbacks/imports completed.

The first failure was not demux seekability, audio clocking, software decode
throughput, ArkTS slider state, or a Vulkan color path. The asynchronous
OHCodec/Vulkan handshake queued frame A into its private one-buffer
`OH_ConsumerSurface` and correctly returned `DeferredUntilRedraw`. Before the
consumer callback retried A, Player published frame B as its sole current
snapshot. The redraw therefore submitted B; interop rejected B because A was
still the exact queued key. No later call could reacquire A, so the consumer
queue remained occupied indefinitely. Seek made the state permanent by
invalidating Player's snapshot without canceling the interop's queued A.

## Root repair

- Player keeps one immutable deferred frame per `VideoRenderAPI` key using
  C++17 atomic shared-pointer publication. `DeferredUntilRedraw` retries that
  exact frame even after a newer frame becomes current. Timer-backoff busy
  results retain their existing latest-frame mailbox/supersession behavior.
- Once the retained frame is consumed, Player requests another redraw if a
  newer snapshot is already current. This covers paused accurate seek as well
  as continuous playback.
- Every presentation-generation change clears retained retries and invokes the
  new non-blocking `VideoRenderAPI::invalidatePendingFrames()` hook. The hook is
  forwarded through the mobile selector and OHOS Vulkan adapter to the Vulkan
  hardware interop.
- OHCodec Vulkan marks its one queued output invalidated. If its consumer buffer
  is already available it is acquired and returned immediately; otherwise the
  native callback performs that drain when the buffer arrives. Only after the
  exact old key is cleared can a new output be queued.
- Player invokes invalidation again when generation changes during a backend
  call, closing the race where an old render attempt queues its producer just
  after seek publishes the new generation.
- Stressing consecutive forward/backward seeks exposed that invalidation alone
  was insufficient: OHCodec can cancel a surface output during decoder flush
  without publishing the callback which would normally drain it. A preliminary
  callback-before-flush wait reproduced `seek.error: Operation timed out`,
  proving that ordering circular.
- A second diagnostic ordering which joined old native render calls before
  flush was also rejected on-device. Wednesday's backward seek left
  `ReleaseOutputBuffer` and `QueueInputBuffer` IPC calls blocked for five
  seconds and triggered HarmonyOS `THREAD_BLOCK_6S`; the system killed the
  foreground process. The codec flush must be allowed to break that native
  dependency, not wait behind it.
- The final C++ contract is a non-joining two-phase protocol. Player first
  publishes non-blocking invalidation, flushes the hardware decoder, publishes
  the completed cancellation generation, and calls
  `completePendingFrameInvalidation()`. OHCodec acquires and releases an
  available invalidated buffer, or clears the association known to have been
  cancelled by the synchronous flush. A retired render call which returns
  after the first pass repeats invalidation and completion, closing the
  late-republish race. A later callback without an association is drained as
  an orphan and cannot poison the next exact frame.
- The remaining freeze was a callback lock cycle. OHOS can invoke the private
  Surface callback before `OH_VideoDecoder_RenderOutputBuffer()` returns. The
  adapter and mobile selector used to reacquire renderer-state locks which the
  render thread still held around that native call; the callback waited for the
  render thread while the next producer operation waited for callback return.
  Redraw forwarding now uses a separate event lock and atomically published
  active renderer API/generation, so it never enters those state locks.
- An early callback and the producer-return path also used to schedule two
  redraws for the same exactly-once OHCodec output. The second present was a
  successful no-op and could never produce the callback needed to continue.
  Interop now records callbacks while the producer call is in flight and emits
  exactly one redraw after it returns. Orphan acquisition remains on the Vulkan
  render thread because a callback may precede the buffer becoming acquirable.
- `Player::seek()` now accepts the latest request during asynchronous input
  opening without changing the interrupt epoch which protects that open. The
  request is clamped and executed after metadata is available, and its callback
  receives failure if opening cannot complete. The OHOS Demo forwards the
  request to this core contract instead of rejecting it from a stale ArkTS
  `seekable` snapshot.

There is no per-frame GPU wait, decoded-source map, software transfer, staging
copy, upload, decoder fallback, or ArkTS/caller-owned retry-frame state. GPU
submissions already in flight retain their existing timeline-based lifetime.

## Deterministic validation

- Windows shared Release `qtav_core_playback` passed. Its new scripted renderer
  defers A, allows B to become current, verifies that the next attempt still has
  A's sequence/timestamp, then verifies the framework automatically schedules
  B after A is consumed.
- The same test blocks a backend render call, seeks concurrently, verifies that
  pending-frame invalidation is delivered without waiting for the render call,
  and verifies the overlapping completion is `FrameDiscarded`.
- OHOS arm64/API 23 shared and static targets built for core, mobile selector,
  Vulkan renderer/adapter, and OHCodec Vulkan interop.
- The OHOS player native library linked and the signed debug HAP packaged at
  `modern/examples/ohos/player-hap/entry/build/default/outputs/default/entry-default-signed.hap`.

The final signed HAP was overwrite-installed on the connected ALN-AL80. Both
document-picker cold starts omitted the former `当前媒体不可定位` toast, and
both processes survived the complete matrix:

| Media / cell | Position | FPS | Hardware decoded | Presented | Native release/callback |
|---|---:|---:|---:|---:|---:|
| `legend.mkv` cold | 0:10 / 45:44 | 25.1 | 257 | 254 | 255/255 |
| `legend.mkv` forward | 30:15 / 45:44 | 25.1 | 769 | 766 | 767/767 |
| `legend.mkv` backward | 6:05 / 45:44 | 25.0 | 1,312 | 1,307 | 1,308/1,308 |
| `wednesday.mp4` cold | 0:10 / 59:28 | 24.1 | 244 | 239 | 240/240 |
| `wednesday.mp4` forward | 39:15 / 59:28 | 24.1 | 787 | 782 | 783/783 |
| `wednesday.mp4` backward | 7:49 / 59:28 | 24.0 | 1,254 | 1,247 | 1,249/1,249 |

An additional Wednesday stress run completed eight forward/backward pairs.
After the last seek it remained Loaded at 24.1 FPS; hardware decode/presentation
rose from 2,163/2,113 to 2,354/2,304 during the next five seconds, native
release and callback stayed balanced at 2,311/2,311, and Player drops remained
two. The final process emitted no seek timeout, media error, `THREAD_BLOCK`, or
crash evidence.
