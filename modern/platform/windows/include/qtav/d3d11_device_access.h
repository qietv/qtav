// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/d3d11_device_access.h is available only on Windows"
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11.h>

#include <memory>

#include <qtav/platform_windows_export.h>

namespace qtav {

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

private:
    friend class D3D11DeviceAccess;

    D3D11ContextGuard(
        std::shared_ptr<void> lifetime,
        void* mutex);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Retains one D3D11 device and its immediate context. Components which share
// this value must hold contextGuard() while issuing immediate-context calls.
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

private:
    class Impl;

    explicit D3D11DeviceAccess(std::shared_ptr<Impl> impl);

    std::shared_ptr<Impl> impl_;
};

} // namespace qtav
