// FSR upscaling on Direct3D 12 for the Direct3D 11 game; fsr_upscaler.hpp describes the route.
#include "fsr_upscaler.hpp"
#include "fidelityfx.hpp"
#include "upscaler.hpp"

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <api/include/dx12/ffx_api_dx12.h>
#include <upscalers/include/ffx_upscale.h>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace days_gone_dlss::fsr_upscaler
{
namespace
{
using Microsoft::WRL::ComPtr;

// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
constexpr std::size_t ListsInFlight = 3;
constexpr DWORD FenceTimeout = 10'000;
// UE4's projection puts the far plane at infinity; FSR still takes a distance. 10 km, in centimetres.
constexpr float FarPlane = 1.0e6f;
// UE4's units are centimetres.
constexpr float MetresPerUnit = 0.01f;

void check(HRESULT result, const char* what)
{
    if (SUCCEEDED(result)) return;

    char text[32]{};
    std::snprintf(text, sizeof(text), " failed (0x%08lX).", static_cast<unsigned long>(result));
    throw std::runtime_error(what + std::string(text));
}

struct HandleCloser
{
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};

// Direct3D 12's view of a texture Direct3D 11 shares, opened again when the upscaler replaces it.
// Holding the texture keeps its address from being reused by another.
struct SharedTexture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D12Resource> resource;
};

// What the FSR context was created for.
struct ContextKey
{
    UINT renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    bool hdr{};

    bool operator==(const ContextKey&) const = default;
};

struct State
{
    const fidelityfx::Api* api{};

    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> fence11;

    // Native: ReShade wraps the device it creates, and other add-ons would see this one as a game
    // device. The proxy stays alive while its device is used.
    ComPtr<ID3D12Device> wrapper, device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence12;
    std::array<ComPtr<ID3D12CommandAllocator>, ListsInFlight> allocators;
    std::array<UINT64, ListsInFlight> submitted{};
    ComPtr<ID3D12GraphicsCommandList> list;
    std::unique_ptr<void, HandleCloser> event;
    UINT64 fenceValue{}, lists{};

    ffxContext context{};
    ContextKey key;
    std::string version;
    SharedTexture color, depth, motion, reactive, output;
};
std::unique_ptr<State> state;

void logMessage(uint32_t type, const wchar_t* message)
{
    std::string text(type == FFX_API_MESSAGE_TYPE_ERROR ? "FSR error: " : "FSR warning: ");
    for (const wchar_t* c = message; c && *c; ++c) text += *c < 128 ? static_cast<char>(*c) : '?';
    upscaler::log(text);
}

template<class Interface> ComPtr<Interface> openShared(ID3D12Device* device, HANDLE handle, const char* what)
{
    ComPtr<Interface> opened;
    const auto result = device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);
    check(result, what);
    return opened;
}

ID3D12Resource* open(State& s, SharedTexture& shared, ID3D11Texture2D* texture, const char* what)
{
    if (shared.texture.Get() == texture) return shared.resource.Get();

    ComPtr<IDXGIResource1> resource;
    check(texture->QueryInterface(IID_PPV_ARGS(&resource)), what);
    HANDLE handle{};
    check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle), what);
    shared.resource = openShared<ID3D12Resource>(s.device12.Get(), handle, what);
    shared.texture = texture;

    return shared.resource.Get();
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

void waitFor(State& s, UINT64 value)
{
    if (s.fence12->GetCompletedValue() >= value) return;

    check(s.fence12->SetEventOnCompletion(value, s.event.get()), "Waiting for Direct3D 12");
    if (WaitForSingleObject(s.event.get(), FenceTimeout) != WAIT_OBJECT_0)
        throw std::runtime_error("Direct3D 12 did not finish FSR within 10 seconds.");
}

void destroyContext(State& s)
{
    if (!s.context) return;

    // FSR's resources may still be in use by the last upscale.
    waitFor(s, s.fenceValue);
    s.api->destroyContext(&s.context, nullptr);
    s.context = nullptr;
}

// FSR keeps its history at the sizes it was created for, so a new mode or HDR needs a new context.
void ensureContext(State& s, const ContextKey& key)
{
    if (s.context && s.key == key) return;

    destroyContext(s);
    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (key.hdr) create.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    create.maxRenderSize = {key.renderWidth, key.renderHeight};
    create.maxUpscaleSize = {key.outputWidth, key.outputHeight};
    create.fpMessage = &logMessage;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = s.device12.Get();
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    create.header.pNext = &backend.header;
    backend.header.pNext = &version.header;
    fidelityfx::check(s.api->createContext(&s.context, &create.header, nullptr), "Creating the FSR context");
    s.key = key;

    const auto running = fidelityfx::version(*s.api, s.context);
    if (running != s.version) upscaler::log("FSR runs version " + (running.empty() ? std::string("unknown") : running) + ".");
    s.version = running;
}

UINT64 submit(State& s, UINT64 waitValue)
{
    check(s.list->Close(), "Recording FSR");
    check(s.queue->Wait(s.fence12.Get(), waitValue), "Waiting for the FSR inputs");
    ID3D12CommandList* lists[]{s.list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    const auto done = ++s.fenceValue;
    check(s.queue->Signal(s.fence12.Get(), done), "Signalling the FSR output");
    return done;
}
}

void start(ID3D11Device* device, const std::filesystem::path& directory)
{
    if (state) return;

    auto s = std::make_unique<State>();
    s->api = &fidelityfx::load(directory);
    ComPtr<ID3D11Device5> device11;
    check(device->QueryInterface(IID_PPV_ARGS(&device11)), "Finding Direct3D 11.4");
    ComPtr<ID3D11DeviceContext> context;
    device11->GetImmediateContext(context.GetAddressOf());
    check(context.As(&s->context11), "Finding the Direct3D 11.4 context");
    check(device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s->fence11)), "Creating the shared fence");

    ComPtr<IDXGIDevice> dxgi;
    check(device11.As(&dxgi), "Finding the DXGI device");
    ComPtr<IDXGIAdapter> adapter;
    check(dxgi->GetAdapter(adapter.GetAddressOf()), "Finding the GPU");
    ComPtr<ID3D12Device> created;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created)), "Creating the Direct3D 12 device");
    if (SUCCEEDED(created->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(s->device12.GetAddressOf())))) s->wrapper = created;
    else s->device12 = created;

    const D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_DIRECT};
    check(s->device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&s->queue)), "Creating the Direct3D 12 queue");
    for (auto& allocator : s->allocators)
        check(s->device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Creating a command allocator");
    check(s->device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->allocators[0].Get(), nullptr, IID_PPV_ARGS(&s->list)),
        "Creating the command list");
    check(s->list->Close(), "Closing the command list");
    HANDLE fence{};
    check(s->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence), "Sharing the fence");
    s->fence12 = openShared<ID3D12Fence>(s->device12.Get(), fence, "Opening the shared fence");
    s->event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!s->event) throw std::runtime_error("Creating the fence event failed.");

    state = std::move(s);
    upscaler::log("FSR started on Direct3D 12.");
}

bool started() noexcept
{
    return state != nullptr;
}

void evaluate(const Inputs& inputs)
{
    auto& s = *state;
    D3D11_TEXTURE2D_DESC color{}, output{};
    inputs.color->GetDesc(&color);
    inputs.output->GetDesc(&output);
    const ContextKey key{color.Width, color.Height, output.Width, output.Height, inputs.hdr};
    const bool reset = inputs.reset || !s.context || s.key != key;
    ensureContext(s, key);

    // Direct3D 12 starts once Direct3D 11 has drawn the inputs.
    const auto drawn = ++s.fenceValue;
    check(s.context11->Signal(s.fence11.Get(), drawn), "Signalling the FSR inputs");
    s.context11->Flush();

    const auto slot = s.lists++ % ListsInFlight;
    waitFor(s, s.submitted[slot]);
    check(s.allocators[slot]->Reset(), "Resetting a command allocator");
    check(s.list->Reset(s.allocators[slot].Get(), nullptr), "Resetting the command list");
    try
    {
        ID3D12Resource* reads[]{open(s, s.color, inputs.color, "Opening the FSR colour"), open(s, s.depth, inputs.depth, "Opening the FSR depth"),
            open(s, s.motion, inputs.motion, "Opening the FSR motion"), open(s, s.reactive, inputs.reactive, "Opening the FSR reactive mask")};
        auto* written = open(s, s.output, inputs.output, "Opening the FSR output");
        std::array<D3D12_RESOURCE_BARRIER, std::size(reads) + 1> barriers{};
        for (std::size_t i = 0; i < std::size(reads); ++i)
            barriers[i] = transition(reads[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.back() = transition(written, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s.list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = s.list.Get();
        dispatch.color = ffxApiGetResourceDX12(reads[0], FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth = ffxApiGetResourceDX12(reads[1], FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors = ffxApiGetResourceDX12(reads[2], FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.reactive = ffxApiGetResourceDX12(reads[3], FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.output = ffxApiGetResourceDX12(written, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = {inputs.jitter[0], inputs.jitter[1]};
        dispatch.motionVectorScale = {1, 1};
        dispatch.renderSize = {inputs.renderWidth, inputs.renderHeight};
        dispatch.upscaleSize = {output.Width, output.Height};
        // The composites sharpen with RCAS themselves, as for DLSS.
        dispatch.enableSharpening = false;
        dispatch.frameTimeDelta = inputs.frameTime;
        dispatch.preExposure = 1;
        dispatch.reset = reset;
        dispatch.cameraNear = inputs.cameraNear;
        dispatch.cameraFar = FarPlane;
        dispatch.cameraFovAngleVertical = inputs.verticalFov;
        dispatch.viewSpaceToMetersFactor = MetresPerUnit;
        fidelityfx::check(s.api->dispatch(&s.context, &dispatch.header), "Upscaling with FSR");

        for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        s.list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }
    catch (...)
    {
        // The next frame resets the list, which it can only once closed.
        s.list->Close();
        throw;
    }
    const auto upscaled = submit(s, drawn);
    s.submitted[slot] = upscaled;

    // The composite reads the output.
    check(s.context11->Wait(s.fence11.Get(), upscaled), "Waiting for FSR");
}

void stop() noexcept
{
    if (!state) return;

    try { destroyContext(*state); }
    catch (const std::exception& error) { upscaler::log(error.what()); }
    state.reset();
    upscaler::log("FSR stopped.");
}

std::string name()
{
    return state && !state->version.empty() ? "FSR " + state->version : "FSR";
}
}
