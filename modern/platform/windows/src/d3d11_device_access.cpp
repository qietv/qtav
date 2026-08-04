// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/d3d11_device_access.h>

#include <d3d10_1.h>
#include <wrl/client.h>

#include <mutex>
#include <utility>

#include "d3d11_device_access_internal.h"

namespace qtav {
namespace {

using Microsoft::WRL::ComPtr;

bool sameComObject(IUnknown* first, IUnknown* second) noexcept
{
    if (!first || !second) {
        return false;
    }

    ComPtr<IUnknown> firstIdentity;
    ComPtr<IUnknown> secondIdentity;
    return SUCCEEDED(first->QueryInterface(
               IID_PPV_ARGS(&firstIdentity)))
        && SUCCEEDED(second->QueryInterface(
            IID_PPV_ARGS(&secondIdentity)))
        && firstIdentity.Get() == secondIdentity.Get();
}

} // namespace

BorrowedD3D11Device::BorrowedD3D11Device(ID3D11Device* value) noexcept
    : value_(value)
{
}

ID3D11Device* BorrowedD3D11Device::get() const noexcept
{
    return value_;
}

BorrowedD3D11Device::operator bool() const noexcept
{
    return value_ != nullptr;
}

BorrowedD3D11DeviceContext::BorrowedD3D11DeviceContext(
    ID3D11DeviceContext* value) noexcept
    : value_(value)
{
}

ID3D11DeviceContext* BorrowedD3D11DeviceContext::get() const noexcept
{
    return value_;
}

BorrowedD3D11DeviceContext::operator bool() const noexcept
{
    return value_ != nullptr;
}

class D3D11DeviceAccess::Impl {
public:
    Impl(
        ComPtr<ID3D11Device> device,
        ComPtr<ID3D11DeviceContext> immediateContext)
        : device_(std::move(device))
        , immediateContext_(std::move(immediateContext))
    {
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> immediateContext_;
    std::recursive_mutex contextMutex_;
};

class D3D11ContextGuard::Impl {
public:
    Impl(std::shared_ptr<void> lifetime, void* mutex, bool tryLock)
        : lifetime_(std::move(lifetime))
        , lock_(
              *static_cast<std::recursive_mutex*>(mutex),
              std::defer_lock)
    {
        if (tryLock) {
            const bool locked = lock_.try_lock();
            (void)locked;
        } else {
            lock_.lock();
        }
    }

    std::shared_ptr<void> lifetime_;
    std::unique_lock<std::recursive_mutex> lock_;
};

D3D11ContextGuard::D3D11ContextGuard(
    std::shared_ptr<void> lifetime,
    void* mutex,
    bool tryLock)
    : impl_(std::make_unique<Impl>(
          std::move(lifetime),
          mutex,
          tryLock))
{
}

D3D11ContextGuard::~D3D11ContextGuard() = default;
D3D11ContextGuard::D3D11ContextGuard(D3D11ContextGuard&&) noexcept =
    default;
D3D11ContextGuard& D3D11ContextGuard::operator=(
    D3D11ContextGuard&&) noexcept = default;

D3D11ContextGuard::operator bool() const noexcept
{
    return impl_ && impl_->lock_.owns_lock();
}

std::shared_ptr<D3D11DeviceAccess> D3D11DeviceAccess::create(
    BorrowedD3D11Device device,
    BorrowedD3D11DeviceContext immediateContext)
{
    if (!device || !immediateContext
        || immediateContext.get()->GetType()
            != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return {};
    }

    ComPtr<ID3D11Device> contextDevice;
    immediateContext.get()->GetDevice(&contextDevice);
    if (!sameComObject(device.get(), contextDevice.Get())) {
        return {};
    }

    ComPtr<ID3D11DeviceContext> selectedImmediateContext;
    device.get()->GetImmediateContext(&selectedImmediateContext);
    if (!sameComObject(
            immediateContext.get(),
            selectedImmediateContext.Get())) {
        return {};
    }

    // FFmpeg decoding and GPU rendering can share this immediate context
    // from different worker threads. The application-level recursive mutex
    // serializes their public entry points; native protection also covers
    // context calls performed inside either component before driver dispatch.
    ComPtr<ID3D10Multithread> multithread;
    if (FAILED(immediateContext.get()->QueryInterface(
            IID_PPV_ARGS(&multithread)))) {
        return {};
    }
    multithread->SetMultithreadProtected(TRUE);
    if (!multithread->GetMultithreadProtected()) {
        return {};
    }

    ComPtr<ID3D11Device> retainedDevice = device.get();
    ComPtr<ID3D11DeviceContext> retainedContext =
        immediateContext.get();
    auto impl = std::make_shared<Impl>(
        std::move(retainedDevice),
        std::move(retainedContext));
    return std::shared_ptr<D3D11DeviceAccess>(
        new D3D11DeviceAccess(std::move(impl)));
}

D3D11DeviceAccess::D3D11DeviceAccess(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

D3D11DeviceAccess::~D3D11DeviceAccess() = default;

BorrowedD3D11Device D3D11DeviceAccess::device() const noexcept
{
    return BorrowedD3D11Device(
        impl_ ? impl_->device_.Get() : nullptr);
}

BorrowedD3D11DeviceContext
D3D11DeviceAccess::immediateContext() const noexcept
{
    return BorrowedD3D11DeviceContext(
        impl_ ? impl_->immediateContext_.Get() : nullptr);
}

D3D11ContextGuard D3D11DeviceAccess::contextGuard() const
{
    return D3D11ContextGuard(
        impl_,
        &impl_->contextMutex_,
        false);
}

D3D11ContextGuard D3D11DeviceAccess::tryContextGuard() const
{
    return D3D11ContextGuard(
        impl_,
        &impl_->contextMutex_,
        true);
}

namespace detail {

void D3D11DeviceAccessPrivate::lock(
    D3D11DeviceAccess& access) noexcept
{
    access.impl_->contextMutex_.lock();
}

void D3D11DeviceAccessPrivate::unlock(
    D3D11DeviceAccess& access) noexcept
{
    access.impl_->contextMutex_.unlock();
}

} // namespace detail
} // namespace qtav
