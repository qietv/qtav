// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/d3d11_device_access.h>

#include <d3d10_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "d3d11_device_access_internal.h"

namespace qtav {
namespace {

using Microsoft::WRL::ComPtr;

class ReservedRecursiveMutex {
public:
    struct TryLockResult {
        bool locked = false;
        bool ownerHonorsReservations = false;
    };

    void lock(bool honorReservations = false)
    {
        const auto thread = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(stateMutex_);
        changed_.wait(
            lock,
            [&] {
                return owner_ == thread
                    || (depth_ == 0
                        && (!honorReservations
                            || reservations_.empty()
                            || reservations_.find(thread)
                                != reservations_.end()));
            });
        if (owner_ == thread) {
            ++depth_;
        } else {
            owner_ = thread;
            depth_ = 1;
            ownerHonorsReservations_ = honorReservations;
        }
    }

    TryLockResult tryLock(bool honorReservations = false)
    {
        const auto thread = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (owner_ == thread) {
            ++depth_;
            return { true, ownerHonorsReservations_ };
        }
        if (depth_ != 0
            || (honorReservations && !reservations_.empty()
                && reservations_.find(thread) == reservations_.end())) {
            return { false, depth_ != 0 && ownerHonorsReservations_ };
        }
        owner_ = thread;
        depth_ = 1;
        ownerHonorsReservations_ = honorReservations;
        return { true, ownerHonorsReservations_ };
    }

    TryLockResult tryLockFor(std::chrono::milliseconds timeout)
    {
        const auto thread = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(stateMutex_);
        if (owner_ == thread) {
            ++depth_;
            return { true, ownerHonorsReservations_ };
        }
        if (!changed_.wait_for(
                lock,
                timeout,
                [&] {
                    return depth_ == 0
                        && (reservations_.empty()
                            || reservations_.find(thread)
                                != reservations_.end());
                })) {
            return { false, ownerHonorsReservations_ };
        }
        owner_ = thread;
        depth_ = 1;
        ownerHonorsReservations_ = false;
        return { true, false };
    }

    void unlock() noexcept
    {
        bool released = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (owner_ != std::this_thread::get_id() || depth_ == 0) {
                std::terminate();
            }
            --depth_;
            if (depth_ == 0) {
                owner_ = {};
                ownerHonorsReservations_ = false;
                released = true;
            }
        }
        if (released) {
            changed_.notify_all();
        }
    }

    void reserve(std::thread::id thread)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        ++reservations_[thread];
    }

    void release(std::thread::id thread) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto found = reservations_.find(thread);
            if (found == reservations_.end()) {
                std::terminate();
            }
            if (--found->second == 0) {
                reservations_.erase(found);
            }
        }
        changed_.notify_all();
    }

private:
    std::mutex stateMutex_;
    std::condition_variable changed_;
    std::thread::id owner_;
    std::size_t depth_ = 0;
    bool ownerHonorsReservations_ = false;
    std::unordered_map<std::thread::id, std::size_t> reservations_;
};

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
    ReservedRecursiveMutex contextMutex_;
};

class D3D11ContextGuard::Impl {
public:
    Impl(
        std::shared_ptr<void> lifetime,
        void* mutex,
        bool tryLock,
        std::chrono::milliseconds timeout,
        bool honorReservations)
        : lifetime_(std::move(lifetime))
        , mutex_(static_cast<ReservedRecursiveMutex*>(mutex))
    {
        if (tryLock) {
            const auto result = honorReservations
                ? mutex_->tryLockFor(timeout)
                : mutex_->tryLock();
            locked_ = result.locked;
            contendedByReservationAwareOwner_ =
                !result.locked && result.ownerHonorsReservations;
        } else {
            mutex_->lock();
            locked_ = true;
        }
    }

    ~Impl()
    {
        if (locked_) {
            mutex_->unlock();
        }
    }

    std::shared_ptr<void> lifetime_;
    ReservedRecursiveMutex* mutex_ = nullptr;
    bool locked_ = false;
    bool contendedByReservationAwareOwner_ = false;
};

D3D11ContextGuard::D3D11ContextGuard(
    std::shared_ptr<void> lifetime,
    void* mutex,
    bool tryLock,
    std::chrono::milliseconds timeout,
    bool honorReservations)
    : impl_(std::make_unique<Impl>(
          std::move(lifetime),
          mutex,
          tryLock,
          timeout,
          honorReservations))
{
}

D3D11ContextGuard::~D3D11ContextGuard() = default;
D3D11ContextGuard::D3D11ContextGuard(D3D11ContextGuard&&) noexcept =
    default;
D3D11ContextGuard& D3D11ContextGuard::operator=(
    D3D11ContextGuard&&) noexcept = default;

D3D11ContextGuard::operator bool() const noexcept
{
    return impl_ && impl_->locked_;
}

bool D3D11ContextGuard::contendedByReservationAwareOwner() const noexcept
{
    return impl_ && impl_->contendedByReservationAwareOwner_;
}

class D3D11ContextReservation::Impl {
public:
    Impl(std::shared_ptr<void> lifetime, void* mutex)
        : lifetime_(std::move(lifetime))
        , mutex_(static_cast<ReservedRecursiveMutex*>(mutex))
        , thread_(std::this_thread::get_id())
    {
        mutex_->reserve(thread_);
    }

    ~Impl()
    {
        mutex_->release(thread_);
    }

    std::shared_ptr<void> lifetime_;
    ReservedRecursiveMutex* mutex_ = nullptr;
    std::thread::id thread_;
};

D3D11ContextReservation::D3D11ContextReservation(
    std::shared_ptr<void> lifetime,
    void* mutex)
    : impl_(std::make_unique<Impl>(
          std::move(lifetime),
          mutex))
{
}

D3D11ContextReservation::~D3D11ContextReservation() = default;
D3D11ContextReservation::D3D11ContextReservation(
    D3D11ContextReservation&&) noexcept = default;
D3D11ContextReservation& D3D11ContextReservation::operator=(
    D3D11ContextReservation&&) noexcept = default;

D3D11ContextReservation::operator bool() const noexcept
{
    return static_cast<bool>(impl_);
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

D3D11ContextGuard D3D11DeviceAccess::tryContextGuardFor(
    std::chrono::milliseconds timeout) const
{
    return D3D11ContextGuard(
        impl_,
        &impl_->contextMutex_,
        true,
        std::max(timeout, std::chrono::milliseconds { 0 }),
        true);
}

D3D11ContextReservation D3D11DeviceAccess::reserveContext() const
{
    return D3D11ContextReservation(
        impl_,
        &impl_->contextMutex_);
}

namespace detail {

void D3D11DeviceAccessPrivate::lock(
    D3D11DeviceAccess& access) noexcept
{
    access.impl_->contextMutex_.lock(true);
}

void D3D11DeviceAccessPrivate::unlock(
    D3D11DeviceAccess& access) noexcept
{
    access.impl_->contextMutex_.unlock();
}

} // namespace detail
} // namespace qtav
