// The game's stand-in swap chain; presenter.hpp describes the route.
#include "presenter.hpp"
#include "fsr_frame_generation.hpp"

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <reshade.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace days_gone_dlss::presenter
{
namespace
{
using Microsoft::WRL::ComPtr;
using reshade::log::level;

// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
// Where ReShade keeps a swap chain's colour space (SKID_SwapChainColorSpace); its runtime reads it there.
constexpr GUID ColorSpaceData{0x18b57e4, 0x1493, 0x4953, {0xad, 0xf2, 0xde, 0x6d, 0x99, 0xcc, 0x5, 0xe5}};
// IDXGIFactory2::CreateSwapChainForHwnd in the factory's virtual function table.
constexpr std::size_t CreateSwapChainForHwndSlot = 15;
// Command allocators in rotation; the swap chain lets at most three frames queue.
constexpr std::size_t FramesInFlight = 4;
constexpr DWORD FenceTimeout = 10'000;

using CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2* factory, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** chain);

FrameCallback onFrame{};
std::filesystem::path addonDirectory;
Route presentRoute{};
// What the factory's slot held before ours: ReShade's hook.
CreateSwapChainForHwnd nextCreate{};
std::atomic<bool> presenting{};

std::mutex statusMutex;
std::string statusText; // Empty until install()

void setStatus(level severity, std::string text)
{
    reshade::log::message(severity, ("Days Gone DLSS: " + text).c_str());
    std::lock_guard lock(statusMutex);
    statusText = std::move(text);
}

// A failed Direct3D call, with its result for the game when it happens inside a swap chain method.
struct Failure : std::runtime_error
{
    HRESULT result;

    Failure(const std::string& what, HRESULT result) : std::runtime_error(what), result(result) {}
};

void check(HRESULT result, const char* what)
{
    if (SUCCEEDED(result)) return;

    char text[32]{};
    std::snprintf(text, sizeof(text), " failed (0x%08lX).", static_cast<unsigned long>(result));
    throw Failure(what + std::string(text), result);
}

void logFailure(const std::exception& failure)
{
    static bool logged{};
    if (!std::exchange(logged, true)) reshade::log::message(level::error, ("Days Gone DLSS: " + std::string(failure.what())).c_str());
}

// The native object behind a ReShade proxy, or the object itself when ReShade does not wrap it.
template<class Interface> ComPtr<Interface> unwrapped(IUnknown* object)
{
    ComPtr<IUnknown> native;
    if (FAILED(object->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(native.GetAddressOf())))) native = object;
    ComPtr<Interface> result;
    native.As(&result);
    return result;
}

// Streamline's proxy of a native object, which it presents through.
template<class Interface> ComPtr<Interface> upgraded(IUnknown* native, const char* what)
{
    ComPtr<IUnknown> proxy;
    proxy.Attach(frame_generation::upgrade(native));
    ComPtr<Interface> result;
    if (!proxy || FAILED(proxy.As(&result))) throw std::runtime_error(std::string("Streamline did not take ") + what + ".");
    return result;
}

template<class Interface> ComPtr<Interface> openShared(ID3D12Device* device, HANDLE handle, const char* what)
{
    ComPtr<Interface> opened;
    const auto result = device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);
    check(result, what);
    return opened;
}

ComPtr<ID3D12Resource> openTexture(ID3D12Device* device, ID3D11Texture2D* texture, const char* what)
{
    ComPtr<IDXGIResource1> resource;
    check(texture->QueryInterface(IID_PPV_ARGS(&resource)), what);
    HANDLE shared{};
    check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared), what);
    return openShared<ID3D12Resource>(device, shared, what);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

struct HandleCloser
{
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};

// Direct3D 12's view of a texture the upscaler shares, opened again when the upscaler replaces it.
struct SharedInput
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D12Resource> resource;

    ID3D12Resource* open(ID3D12Device* device, ID3D11Texture2D* replacement)
    {
        if (texture.Get() != replacement)
        {
            resource = openTexture(device, replacement, "Opening a frame generation input");
            texture = replacement;
        }
        return resource.Get();
    }
};

// What the game holds in place of its swap chain. The game renders into `backBuffer`; calls that do
// not concern the back buffer pass on to the real swap chain.
class SwapChain final : public IDXGISwapChain4
{
public:
    SwapChain(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& requested,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output);
    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    // ReShade's overlay, drawn into the back buffer at each present. Recreated when the buffers change.
    void createOverlay();
    void destroyOverlay();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject and IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override { return chain->SetPrivateData(name, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* data) override { return chain->SetPrivateDataInterface(name, data); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override { return chain->GetPrivateData(name, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID id, void** parent) override { return chain->GetParent(id, parent); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** device) override;

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT interval, UINT flags) override { return present(interval, flags); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID id, void** surface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override { return chain->SetFullscreenState(fullscreen, target); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override { return chain->GetFullscreenState(fullscreen, target); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* result) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override { return chain->ResizeTarget(mode); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override { return chain->GetContainingOutput(output); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override;

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* result) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* result) override { return chain->GetFullscreenDesc(result); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* result) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID id, void** result) override { return chain->GetCoreWindow(id, result); }
    // Every present copies the whole frame, so dirty rectangles and scrolling do not apply.
    HRESULT STDMETHODCALLTYPE Present1(UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS*) override { return present(interval, flags); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return chain->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override { return chain->GetRestrictToOutput(output); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override { return chain->SetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override { return chain->GetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return chain->SetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override { return chain->GetRotation(rotation); }

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override { return chain->SetSourceSize(width, height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override { return chain->GetSourceSize(width, height); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override { return chain->SetMaximumFrameLatency(latency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override { return chain->GetMaximumFrameLatency(latency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return chain->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override { return chain->SetMatrixTransform(matrix); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override { return chain->GetMatrixTransform(matrix); }

    // IDXGISwapChain3. The game sees one back buffer, as Direct3D 11 shows the current one as buffer 0.
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return 0; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space, UINT* support) override
    {
        return chain->CheckColorSpaceSupport(space, support);
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT*,
        IUnknown* const*) override
    {
        return ResizeBuffers(count, width, height, format, flags);
    }

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) override
    {
        return chain->SetHDRMetaData(type, size, metadata);
    }

private:
    ~SwapChain();

    HRESULT present(UINT interval, UINT flags);
    void copyFrame(const frame_generation::Inputs& inputs);
    void createBackBuffer();
    void waitIdle();
    void waitFor(UINT64 value);

    std::atomic<ULONG> references{1};

    // Direct3D 11: the game's device as it passed it (ReShade's proxy), and the native one the add-on uses.
    ComPtr<IUnknown> gameDevice;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> fence11;

    // Direct3D 12, native: ReShade wraps the device it creates, and other add-ons would see this one as
    // a game device. Streamline's proxies of the device and queue make its swap chain. The copies run
    // on a queue of their own: DLSS Frame Generation blocks the presenting queue while it works, and
    // Direct3D 11's next frame waits only for its copy, not for that. FSR generates on the queue it
    // presents from, after the copy and the preparation it needs, so for FSR that is the copy queue.
    ComPtr<ID3D12Device> wrappedDevice, device12, streamlineDevice;
    ComPtr<ID3D12CommandQueue> streamlineQueue, presentQueue, copyQueue;
    ComPtr<ID3D12Fence> fence12;
    std::array<ComPtr<ID3D12CommandAllocator>, FramesInFlight> allocators;
    std::array<UINT64, FramesInFlight> submitted{};
    ComPtr<ID3D12GraphicsCommandList> list;
    std::unique_ptr<void, HandleCloser> fenceEvent;
    UINT64 fenceValue{}, frames{};
    // The game's own presents and when the last one was handed on, for the counts it is shown.
    std::atomic<UINT> gamePresents{};
    std::atomic<LONGLONG> lastPresentTime{};

    // Streamline's proxy of the real swap chain, or AMD's swap chain.
    ComPtr<IDXGISwapChain4> chain;
    HWND window{};
    DXGI_SWAP_CHAIN_DESC1 desc{}; // As the game sees it
    DXGI_COLOR_SPACE_TYPE colorSpace{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    reshade::api::effect_runtime* overlay{};
    bool creatingOverlay{};

    // The game renders here; Direct3D 12 copies from its view of the same texture.
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D12Resource> sharedBackBuffer;
    SharedInput depth, motion, hudless;
};

SwapChain::SwapChain(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& requested,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output)
    : gameDevice(device), window(hwnd), desc(requested)
{
    device11 = unwrapped<ID3D11Device5>(device);
    if (!device11) throw std::runtime_error("The game's device lacks Direct3D 11.4.");
    ComPtr<ID3D11DeviceContext> context;
    device11->GetImmediateContext(context.GetAddressOf());
    check(context.As(&context11), "Finding the Direct3D 11.4 context");
    check(device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "Creating the shared fence");

    ComPtr<IDXGIDevice> dxgiDevice;
    check(device11.As(&dxgiDevice), "Finding the DXGI device");
    ComPtr<IDXGIAdapter> adapter;
    check(dxgiDevice->GetAdapter(adapter.GetAddressOf()), "Finding the GPU");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&wrappedDevice)), "Creating the Direct3D 12 device");
    device12 = unwrapped<ID3D12Device>(wrappedDevice.Get());
    const D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (presentRoute == Route::Streamline)
    {
        if (!frame_generation::attach(device12.Get())) throw std::runtime_error("Streamline did not take the Direct3D 12 device.");
        streamlineDevice = upgraded<ID3D12Device>(device12.Get(), "the Direct3D 12 device");
        check(streamlineDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&streamlineQueue)), "Creating the Direct3D 12 queue");
        ComPtr<IUnknown> nativeQueue;
        nativeQueue.Attach(frame_generation::nativeOf(streamlineQueue.Get()));
        if (!nativeQueue || FAILED(nativeQueue.As(&presentQueue))) throw std::runtime_error("Finding the native Direct3D 12 queue failed.");
    }
    check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyQueue)), "Creating the Direct3D 12 copy queue");
    if (presentRoute == Route::Fsr) presentQueue = copyQueue;
    HANDLE sharedFence{};
    check(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedFence), "Sharing the fence");
    fence12 = openShared<ID3D12Fence>(device12.Get(), sharedFence, "Opening the shared fence");
    for (auto& allocator : allocators)
        check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Creating a command allocator");
    check(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list)),
        "Creating the command list");
    check(list->Close(), "Closing the command list");
    fenceEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!fenceEvent) throw std::runtime_error("Creating the fence event failed.");

    // Direct3D 12 presents only with flip model, and the copies need nothing beyond output. Through
    // Streamline's factory the real swap chain is Streamline's to present.
    auto real = desc;
    real.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    real.BufferCount = std::max(desc.BufferCount, 2u);
    if (real.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) real.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (presentRoute == Route::Streamline)
    {
        const auto streamlineFactory = upgraded<IDXGIFactory2>(factory, "the DXGI factory");
        ComPtr<IDXGISwapChain1> created;
        check(streamlineFactory->CreateSwapChainForHwnd(streamlineQueue.Get(), window, &real, fullscreen, output, created.GetAddressOf()),
            "Creating the Direct3D 12 swap chain");
        check(created.As(&chain), "Finding IDXGISwapChain4");
    }
    else chain.Attach(fsr_frame_generation::createSwapChain(factory, presentQueue.Get(), window, real, fullscreen));

    // A width or height of 0 takes the window's.
    DXGI_SWAP_CHAIN_DESC1 actual{};
    check(chain->GetDesc1(&actual), "Reading the swap chain's size");
    desc.Width = actual.Width;
    desc.Height = actual.Height;
    createBackBuffer();
}

SwapChain::~SwapChain()
{
    destroyOverlay();
    try { waitIdle(); }
    catch (const Failure& failure) { logFailure(failure); }

    // DXGI refuses to release a swap chain in exclusive fullscreen.
    BOOL fullscreen{};
    if (SUCCEEDED(chain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen) chain->SetFullscreenState(FALSE, nullptr);
    // Streamline's proxies go before Streamline, and Streamline before the devices it was given. FSR's
    // contexts go before its swap chain.
    if (presentRoute == Route::Fsr) fsr_frame_generation::stop();
    chain.Reset();
    streamlineQueue.Reset();
    streamlineDevice.Reset();
    if (presentRoute == Route::Streamline) frame_generation::stop();
    presenting = false;
    setStatus(level::info, "The game released its swap chain.");
}

void SwapChain::createOverlay()
{
    // ReShade's runtime wants the device the swap chain reports; it gets the native one, as for the
    // swap chains ReShade hooks itself.
    char base[MAX_PATH]{};
    auto size = sizeof(base);
    reshade::get_reshade_base_path(base, &size);
    const auto config = std::string(base) + "\\ReShade.ini";
    creatingOverlay = true;
    const bool created = reshade::create_effect_runtime(reshade::api::device_api::d3d11, device11.Get(), context11.Get(),
        static_cast<IDXGISwapChain*>(this), config.c_str(), &overlay);
    creatingOverlay = false;
    if (!created) reshade::log::message(level::error, "Days Gone DLSS: ReShade could not draw its overlay on the game's frames.");
}

void SwapChain::destroyOverlay()
{
    if (overlay) reshade::destroy_effect_runtime(std::exchange(overlay, nullptr));
}

HRESULT SwapChain::QueryInterface(REFIID id, void** object)
{
    if (!object) return E_POINTER;

    if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIObject) || id == __uuidof(IDXGIDeviceSubObject) || id == __uuidof(IDXGISwapChain)
        || id == __uuidof(IDXGISwapChain1) || id == __uuidof(IDXGISwapChain2) || id == __uuidof(IDXGISwapChain3)
        || id == __uuidof(IDXGISwapChain4))
    {
        AddRef();
        *object = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG SwapChain::Release()
{
    const auto left = --references;
    if (!left) delete this;
    return left;
}

HRESULT SwapChain::GetDevice(REFIID id, void** device)
{
    return creatingOverlay ? device11->QueryInterface(id, device) : gameDevice->QueryInterface(id, device);
}

HRESULT SwapChain::GetBuffer(UINT index, REFIID id, void** surface)
{
    if (!surface) return DXGI_ERROR_INVALID_CALL;

    *surface = nullptr;
    if (index != 0 || !backBuffer) return DXGI_ERROR_INVALID_CALL;
    return backBuffer->QueryInterface(id, surface);
}

HRESULT SwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* result)
{
    if (!result) return DXGI_ERROR_INVALID_CALL;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
    if (const auto status = chain->GetFullscreenDesc(&fullscreen); FAILED(status)) return status;
    *result = {};
    result->BufferDesc = {desc.Width, desc.Height, fullscreen.RefreshRate, desc.Format, fullscreen.ScanlineOrdering, fullscreen.Scaling};
    result->SampleDesc = desc.SampleDesc;
    result->BufferUsage = desc.BufferUsage;
    result->BufferCount = desc.BufferCount;
    result->OutputWindow = window;
    result->Windowed = fullscreen.Windowed;
    result->SwapEffect = desc.SwapEffect;
    result->Flags = desc.Flags;
    return S_OK;
}

HRESULT SwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* result)
{
    if (!result) return DXGI_ERROR_INVALID_CALL;

    *result = desc;
    return S_OK;
}

HRESULT SwapChain::GetHwnd(HWND* result)
{
    if (!result) return DXGI_ERROR_INVALID_CALL;

    *result = window;
    return S_OK;
}

// 1.07 times each frame by waiting for its present's count in the frame statistics, and its frame
// limiter sleeps by those times. Frame generation presents several times per game frame, and at 2x
// the count it waited for never showed: every frame waited out a timeout of about 0.4 s. The counts
// are the game's own presents, each shown once handed on.
HRESULT SwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics)
{
    if (!statistics) return DXGI_ERROR_INVALID_CALL;

    if (FAILED(chain->GetFrameStatistics(statistics))) *statistics = {};
    statistics->PresentCount = gamePresents;
    statistics->SyncQPCTime.QuadPart = lastPresentTime;
    return S_OK;
}

HRESULT SwapChain::GetLastPresentCount(UINT* count)
{
    if (!count) return DXGI_ERROR_INVALID_CALL;

    *count = gamePresents;
    return S_OK;
}

HRESULT SwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE space)
{
    const auto result = chain->SetColorSpace1(space);
    if (FAILED(result) || space == colorSpace) return result;

    // ReShade's overlay reads the colour space when it starts, as for a resize.
    colorSpace = space;
    destroyOverlay();
    chain->SetPrivateData(ColorSpaceData, sizeof(space), &space);
    createOverlay();
    return result;
}

HRESULT SwapChain::ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    try
    {
        // Direct3D 12 resizes only once nothing uses the old buffers, ReShade's overlay included.
        destroyOverlay();
        waitIdle();
        if (presentRoute == Route::Fsr) fsr_frame_generation::release();
        backBuffer.Reset();
        sharedBackBuffer.Reset();
        const auto result = chain->ResizeBuffers(count ? std::max(count, 2u) : 0, width, height, format, flags);
        if (SUCCEEDED(result))
        {
            DXGI_SWAP_CHAIN_DESC1 actual{};
            check(chain->GetDesc1(&actual), "Reading the swap chain's size");
            desc.Width = actual.Width;
            desc.Height = actual.Height;
            if (count) desc.BufferCount = count;
            if (format != DXGI_FORMAT_UNKNOWN) desc.Format = format;
            desc.Flags = flags;
        }
        // The game gets a back buffer either way, at the old size when resizing failed.
        createBackBuffer();
        createOverlay();
        return result;
    }
    catch (const Failure& failure)
    {
        logFailure(failure);
        return failure.result;
    }
}

HRESULT SwapChain::present(UINT interval, UINT flags)
{
    // A test present only asks whether the window is visible; nothing was rendered for it.
    if (flags & DXGI_PRESENT_TEST) return chain->Present(interval, flags);

    const auto inputs = onFrame ? onFrame(backBuffer.Get()) : frame_generation::Inputs{};
    if (overlay) reshade::update_and_present_effect_runtime(overlay);
    try { copyFrame(inputs); }
    catch (const Failure& failure)
    {
        logFailure(failure);
        return failure.result;
    }
    frame_generation::beginPresent(inputs);
    const auto result = chain->Present(interval, flags);
    LARGE_INTEGER presented{};
    QueryPerformanceCounter(&presented);
    lastPresentTime = presented.QuadPart;
    ++gamePresents;
    frame_generation::endPresent(inputs);
    return result;
}

void SwapChain::copyFrame(const frame_generation::Inputs& inputs)
{
    const auto rendered = ++fenceValue;
    check(context11->Signal(fence11.Get(), rendered), "Signalling the rendered frame");
    context11->Flush();

    const auto slot = frames++ % FramesInFlight;
    waitFor(submitted[slot]);
    check(allocators[slot]->Reset(), "Resetting a command allocator");
    check(list->Reset(allocators[slot].Get(), nullptr), "Resetting the command list");
    // Streamline chooses the back buffer; frame generation keeps some of them.
    ComPtr<ID3D12Resource> target;
    check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target)), "Finding the swap chain's back buffer");
    D3D12_RESOURCE_BARRIER barriers[]{
        transition(sharedBackBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    list->ResourceBarrier(2, barriers);
    list->CopyResource(target.Get(), sharedBackBuffer.Get());
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers);
    try
    {
        ID3D12Resource *depthResource{}, *motionResource{}, *hudlessResource{};
        if (inputs.valid)
        {
            depthResource = depth.open(device12.Get(), inputs.depth);
            motionResource = motion.open(device12.Get(), inputs.motion);
            hudlessResource = hudless.open(device12.Get(), inputs.hudless);
        }
        // FSR also hears of frames without inputs, to stop generating.
        if (presentRoute == Route::Fsr)
        {
            const bool hdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            fsr_frame_generation::record(list.Get(), inputs, depthResource, motionResource, hudlessResource, hdr);
        }
        else if (inputs.valid) frame_generation::tag(list.Get(), inputs, depthResource, motionResource, hudlessResource);
    }
    catch (const Failure& failure) { logFailure(failure); }
    check(list->Close(), "Recording the copy");

    check(copyQueue->Wait(fence12.Get(), rendered), "Waiting for the rendered frame");
    ID3D12CommandList* lists[]{list.Get()};
    copyQueue->ExecuteCommandLists(1, lists);
    const auto copied = ++fenceValue;
    check(copyQueue->Signal(fence12.Get(), copied), "Signalling the copied frame");
    submitted[slot] = copied;
    // The game's next frame draws into the same textures; the frame is presented once copied.
    check(context11->Wait(fence11.Get(), copied), "Waiting for the copied frame");
    check(presentQueue->Wait(fence12.Get(), copied), "Waiting to present the copied frame");
}

void SwapChain::createBackBuffer()
{
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = desc.Width;
    texture.Height = desc.Height;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = desc.Format;
    texture.SampleDesc.Count = 1;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (desc.BufferUsage & DXGI_USAGE_SHADER_INPUT) texture.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) texture.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    check(device11->CreateTexture2D(&texture, nullptr, backBuffer.ReleaseAndGetAddressOf()), "Creating the shared back buffer");
    sharedBackBuffer = openTexture(device12.Get(), backBuffer.Get(), "Opening the shared back buffer");
}

// Both devices and both queues finish everything given so far.
void SwapChain::waitIdle()
{
    const auto rendered = ++fenceValue;
    check(context11->Signal(fence11.Get(), rendered), "Signalling Direct3D 11's work");
    context11->Flush();
    check(copyQueue->Wait(fence12.Get(), rendered), "Waiting for Direct3D 11's work");
    const auto copied = ++fenceValue;
    check(copyQueue->Signal(fence12.Get(), copied), "Signalling the copy queue's work");
    check(presentQueue->Wait(fence12.Get(), copied), "Waiting for the copy queue's work");
    const auto done = ++fenceValue;
    check(presentQueue->Signal(fence12.Get(), done), "Signalling Direct3D 12's work");
    waitFor(done);
}

void SwapChain::waitFor(UINT64 value)
{
    if (fence12->GetCompletedValue() >= value) return;

    check(fence12->SetEventOnCompletion(value, fenceEvent.get()), "Waiting for Direct3D 12");
    if (WaitForSingleObject(fenceEvent.get(), FenceTimeout) != WAIT_OBJECT_0)
        throw Failure("Direct3D 12 did not finish a frame within 10 seconds.", DXGI_ERROR_DEVICE_HUNG);
}

HRESULT STDMETHODCALLTYPE createSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** chain)
{
    // Only the game's Direct3D 11 swap chain is replaced; the stand-in's Direct3D 12 one, and any
    // other while it lives, pass on.
    ComPtr<ID3D11Device> game;
    if (presenting || !device || !desc || !chain || FAILED(device->QueryInterface(IID_PPV_ARGS(&game))))
        return nextCreate(factory, device, window, desc, fullscreen, output, chain);

    // Without frame generation the game's own swap chain does the same job without the copy.
    const bool fsr = presentRoute == Route::Fsr;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    const bool started = fsr ? fsr_frame_generation::start(addonDirectory)
        : SUCCEEDED(game.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))
            && frame_generation::start(addonDirectory, adapter.Get());
    if (!started)
    {
        setStatus(level::warning, "Frame generation did not start: " + (fsr ? fsr_frame_generation::status() : frame_generation::status()));
        return nextCreate(factory, device, window, desc, fullscreen, output, chain);
    }
    SwapChain* created{};
    try
    {
        created = new SwapChain(factory, device, window, *desc, fullscreen, output);
    }
    catch (const std::exception& error)
    {
        if (fsr) fsr_frame_generation::stop();
        else frame_generation::stop();
        setStatus(level::warning, std::string("Frame generation did not start: ") + error.what());
        return nextCreate(factory, device, window, desc, fullscreen, output, chain);
    }
    created->createOverlay();
    *chain = created;
    presenting = true;
    setStatus(level::info, "Presenting through Direct3D 12 for frame generation.");
    return S_OK;
}
}

void install(FrameCallback frame, const std::filesystem::path& directory, Route route) noexcept
{
    static bool installed{};
    if (std::exchange(installed, true)) return;

    onFrame = frame;
    addonDirectory = directory;
    presentRoute = route;
    try
    {
        // DXGI's factories share one function table, which ReShade has hooked by now. Taking the slot
        // over puts this hook in front of ReShade's.
        ComPtr<IDXGIFactory2> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Creating a DXGI factory");
        auto* slot = &(*reinterpret_cast<void***>(factory.Get()))[CreateSwapChainForHwndSlot];
        DWORD protection{};
        if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
            throw std::runtime_error("Making the DXGI factory's function table writable failed.");
        nextCreate = reinterpret_cast<CreateSwapChainForHwnd>(*slot);
        *slot = reinterpret_cast<void*>(&createSwapChainForHwnd);
        VirtualProtect(slot, sizeof(*slot), protection, &protection);
        setStatus(level::info, "Waiting for the game's swap chain.");
    }
    catch (const std::exception& error)
    {
        setStatus(level::error, std::string("Frame generation did not start: ") + error.what());
    }
}

bool active() noexcept
{
    return presenting;
}

Route route() noexcept
{
    return presentRoute;
}

std::string status()
{
    std::lock_guard lock(statusMutex);
    return statusText;
}
}
