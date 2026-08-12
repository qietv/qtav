# OHOS deferred-render seek investigation and repair

Date: 2026-08-12

## Symptom and evidence

The user-facing OHOS player could continue audio and media-position progress
while its picture remained frozen after seeking forward and then backward. A
captured `legend.mkv` session advanced from roughly 2:52 to 3:25 and continued
receiving OHCodec video outputs, while successful presentation FPS stayed at
zero. Only the first few private-consumer callbacks/imports completed.

The failure was not demux seekability, audio clocking, software decode
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

The HAP was not installed or launched in this repair turn. Connected-device
replay with local `legend.mkv` and `wednesday.mp4` remains gated on explicit
installation approval.
