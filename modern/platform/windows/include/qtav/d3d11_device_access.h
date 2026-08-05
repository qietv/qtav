// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/d3d11_device_access.h is available only on Windows"
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11.h>

#include <chrono>
#include <memory>

#include <qtav/platform_windows_export.h>

namespace qtav {

namespace detail {
class D3D11DeviceAccessPrivate;
}

class QTAV_PLATFORM_WINDOWS_EXPORT BorrowedD3D11Device final {
public:
    explicit BorrowedD3D11Device(ID3D11Device* value = nullptr) noexcept;

    ID3D11Device* get() const noexcept;
    explicit operator bool() const noexcept;

private:
    ID3D11Device* value_ = nullptr;
};

class QTAV_PLATFORM_WINDOWS_EXPORT BorrowedD3D11DeviceContext final {
public:
    explicit BorrowedD3D11DeviceContext(
        ID3D11DeviceContext* value = nullptr) noexcept;

    ID3D11DeviceContext* get() const noexcept;
    explicit operator bool() const noexcept;

private:
    ID3D11DeviceContext* value_ = nullptr;
};

// Move-only ownership of the shared recursive immediate-context lock. The
// associated device access remains alive until the guard is destroyed.
class QTAV_PLATFORM_WINDOWS_EXPORT D3D11ContextGuard final {
public:
    ~D3D11ContextGuard();

    D3D11ContextGuard(D3D11ContextGuard&&) noexcept;
    D3D11ContextGuard& operator=(D3D11ContextGuard&&) noexcept;
    D3D11ContextGuard(const D3D11ContextGuard&) = delete;
    D3D11ContextGuard& operator=(const D3D11ContextGuard&) = delete;

    explicit operator bool() const noexcept;
    // Valid only when the guard is false. Returns true when the failed
    // non-blocking acquisition was contending with a reservation-aware owner,
    // such as FFmpeg's D3D11VA decode callback.
    bool contendedByReservationAwareOwner() const noexcept;

private:
    friend class D3D11DeviceAccess;

    D3D11ContextGuard(
        std::shared_ptr<void> lifetime,
        void* mutex,
        bool tryLock,
        std::chrono::milliseconds timeout = std::chrono::milliseconds { 0 },
        bool honorReservations = false);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Move-only reservation that gives its creating thread priority over new
// FFmpeg decode-side context acquisitions. It does not take the context lock
// and does not block unrelated public contextGuard() users. This lets a
// real-time renderer perform its initial bounded handoff or retry without
// being starved by a decoder that immediately begins its next D3D11 operation.
// Destroy the reservation as soon as the pass succeeds or is cancelled.
class QTAV_PLATFORM_WINDOWS_EXPORT D3D11ContextReservation final {
public:
    ~D3D11ContextReservation();

    D3D11ContextReservation(D3D11ContextReservation&&) noexcept;
    D3D11ContextReservation& operator=(
        D3D11ContextReservation&&) noexcept;
    D3D11ContextReservation(const D3D11ContextReservation&) = delete;
    D3D11ContextReservation& operator=(
        const D3D11ContextReservation&) = delete;

    explicit operator bool() const noexcept;

private:
    friend class D3D11DeviceAccess;

    D3D11ContextReservation(
        std::shared_ptr<void> lifetime,
        void* mutex);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Retains one D3D11 device and its immediate context. Components which share
// this value must hold contextGuard() while issuing immediate-context calls.
// create() also enables native multithread protection on the context and
// returns null when that protection is unavailable.
class QTAV_PLATFORM_WINDOWS_EXPORT D3D11DeviceAccess final {
public:
    static std::shared_ptr<D3D11DeviceAccess> create(
        BorrowedD3D11Device device,
        BorrowedD3D11DeviceContext immediateContext);

    ~D3D11DeviceAccess();

    D3D11DeviceAccess(const D3D11DeviceAccess&) = delete;
    D3D11DeviceAccess& operator=(const D3D11DeviceAccess&) = delete;

    BorrowedD3D11Device device() const noexcept;
    BorrowedD3D11DeviceContext immediateContext() const noexcept;
    D3D11ContextGuard contextGuard() const;
    // Non-blocking form for real-time render paths. The returned guard converts
    // to false when another thread currently owns the immediate context.
    D3D11ContextGuard tryContextGuard() const;
    // Bounded-wait form for a private render worker after it has reserved the
    // context. The returned guard converts to false when the timeout expires.
    D3D11ContextGuard tryContextGuardFor(
        std::chrono::milliseconds timeout) const;
    // Prevents new FFmpeg decode-side acquisitions from overtaking an initial
    // bounded handoff or retry on the calling thread. The reservation itself
    // never owns the context.
    D3D11ContextReservation reserveContext() const;

private:
    friend class detail::D3D11DeviceAccessPrivate;

    class Impl;

    explicit D3D11DeviceAccess(std::shared_ptr<Impl> impl);

    std::shared_ptr<Impl> impl_;
};

} // namespace qtav
