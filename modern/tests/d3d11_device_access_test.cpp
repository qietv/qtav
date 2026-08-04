// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d10_1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <qtav/d3d11_device_access.h>

#include <cassert>
#include <chrono>
#include <future>
#include <iterator>
#include <memory>

namespace {

using Microsoft::WRL::ComPtr;

struct DeviceResources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

DeviceResources makeDevice()
{
    DeviceResources result;
    const D3D_FEATURE_LEVEL levels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_11_0;
    HRESULT status = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &result.device,
        &selected,
        &result.context);
    if (status == E_INVALIDARG) {
        status = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &result.device,
            &selected,
            &result.context);
    }
    assert(SUCCEEDED(status));
    assert(selected >= D3D_FEATURE_LEVEL_11_0);
    return result;
}

} // namespace

int main()
{
    DeviceResources first = makeDevice();
    DeviceResources second = makeDevice();

    qtav::BorrowedD3D11Device borrowedDevice(first.device.Get());
    qtav::BorrowedD3D11DeviceContext borrowedContext(
        first.context.Get());
    assert(borrowedDevice);
    assert(borrowedContext);
    assert(borrowedDevice.get() == first.device.Get());
    assert(borrowedContext.get() == first.context.Get());

    assert(!qtav::D3D11DeviceAccess::create(
        qtav::BorrowedD3D11Device {},
        borrowedContext));
    assert(!qtav::D3D11DeviceAccess::create(
        borrowedDevice,
        qtav::BorrowedD3D11DeviceContext {}));
    assert(!qtav::D3D11DeviceAccess::create(
        borrowedDevice,
        qtav::BorrowedD3D11DeviceContext(second.context.Get())));

    ComPtr<ID3D11DeviceContext> deferredContext;
    assert(SUCCEEDED(first.device->CreateDeferredContext(
        0,
        &deferredContext)));
    assert(!qtav::D3D11DeviceAccess::create(
        borrowedDevice,
        qtav::BorrowedD3D11DeviceContext(deferredContext.Get())));

    auto access = qtav::D3D11DeviceAccess::create(
        borrowedDevice,
        borrowedContext);
    assert(access);
    assert(access->device().get() == first.device.Get());
    assert(access->immediateContext().get() == first.context.Get());
    ComPtr<ID3D10Multithread> multithread;
    assert(SUCCEEDED(first.context.As(&multithread)));
    assert(multithread->GetMultithreadProtected());

    {
        auto outerGuard = access->contextGuard();
        auto recursiveGuard = access->contextGuard();
        auto recursiveTryGuard = access->tryContextGuard();
        assert(outerGuard);
        assert(recursiveGuard);
        assert(recursiveTryGuard);
    }

    std::promise<void> tryWorkerStarted;
    auto tryWorkerStartedFuture = tryWorkerStarted.get_future();
    std::future<bool> tryWorker;
    {
        auto mainGuard = access->contextGuard();
        tryWorker = std::async(
            std::launch::async,
            [&access, &tryWorkerStarted] {
                tryWorkerStarted.set_value();
                auto workerGuard = access->tryContextGuard();
                return static_cast<bool>(workerGuard);
            });
        tryWorkerStartedFuture.wait();
        assert(tryWorker.wait_for(std::chrono::seconds(2))
            == std::future_status::ready);
        assert(!tryWorker.get());
    }

    std::promise<void> workerStarted;
    auto workerStartedFuture = workerStarted.get_future();
    std::future<bool> worker;
    {
        auto mainGuard = access->contextGuard();
        worker = std::async(
            std::launch::async,
            [&access, &workerStarted] {
                workerStarted.set_value();
                auto workerGuard = access->contextGuard();
                return static_cast<bool>(workerGuard);
            });
        workerStartedFuture.wait();
        assert(worker.wait_for(std::chrono::milliseconds(50))
            == std::future_status::timeout);
    }
    assert(worker.wait_for(std::chrono::seconds(2))
        == std::future_status::ready);
    assert(worker.get());

    ID3D11Device* retainedDevice = first.device.Get();
    ID3D11DeviceContext* retainedContext = first.context.Get();
    deferredContext.Reset();
    second.context.Reset();
    second.device.Reset();
    first.context.Reset();
    first.device.Reset();

    assert(access->device().get() == retainedDevice);
    assert(access->immediateContext().get() == retainedContext);
    assert(access->device().get()->GetFeatureLevel()
        >= D3D_FEATURE_LEVEL_11_0);

    {
        auto guard = access->contextGuard();
        access.reset();
        assert(guard);
        assert(retainedDevice->GetFeatureLevel()
            >= D3D_FEATURE_LEVEL_11_0);
    }
    return 0;
}
