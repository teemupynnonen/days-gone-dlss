#include "upscaler.hpp"
#include "upscaler_shaders.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace days_gone_dlss::upscaler
{
namespace
{
using Microsoft::WRL::ComPtr;
using Address = std::uintptr_t;

// Steam build 19221447; docs/DEVELOPMENT.md lists what each address is.
constexpr Address PreVisibilityFrameSetupRva = 0x20E6920;
constexpr unsigned char PreVisibilityPrefix[]{0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
// FMemStackBase page allocation for RHI commands (the slow path of every AllocCommand).
constexpr Address AllocateCommandPageRva = 0x19C0EA0;
constexpr unsigned char AllocateCommandPagePrefix[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
    0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83};
// FRHICommandListBase: CommandLink, bExecuting, NumCommands and the command memory stack.
constexpr std::size_t CommandLinkOffset = 0x08, ExecutingOffset = 0x10, CommandCountOffset = 0x14, CommandMemoryOffset = 0x30;
// TConsoleVariableData pointers for r.ScreenPercentage and r.TemporalAASamples: {game thread, render thread}.
constexpr Address ScreenPercentageDataRva = 0x4ADCA50, TemporalSamplesDataRva = 0x4CD0620;
// FSceneRenderer views and the FViewInfo fields read here.
constexpr std::size_t RendererViews = 0x80, RendererViewCount = 0x88, ViewInfoSize = 0x2D50;
constexpr std::size_t ViewRectOffset = 0xB28, UnscaledViewRectOffset = 0xB38, TemporalJitterOffset = 0x10F0,
    ScreenPercentageOffset = 0x1744, CameraCutOffset = 0x1755, AntiAliasingOffset = 0x1760, ViewStateOffset = 0x1CE0;
constexpr std::uint8_t BendTemporalAA = 3;
constexpr int MaxViews = 4;
// TRefCountPtr to the native ID3D11ComputeShader/ID3D11PixelShader inside Bend's FD3D11 shader objects.
constexpr std::size_t NativeShaderOffset = 0x98;
constexpr char MismatchStatus[] = "bytecode does not match build 19221447";
// Any GUID identifies a custom-engine NGX project.
constexpr char NgxProjectId[] = "8d3b6f2e-5c1a-4e7b-9f0d-2a6c4b8e1d73";

enum class Pass : std::uint8_t { None, TemporalFull, TemporalUpscale, Composite, DepthResolve };

constexpr std::uint8_t nibble(char value) { return static_cast<std::uint8_t>(value <= '9' ? value - '0' : value - 'a' + 10); }
constexpr std::array<std::uint8_t, 16> dxbcHash(const char (&hex)[33])
{
    std::array<std::uint8_t, 16> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint8_t>(nibble(hex[i * 2]) << 4 | nibble(hex[i * 2 + 1]));
    return result;
}

struct ShaderSlot
{
    const char* name;
    Address cacheRva; // The engine's cached global-shader lookup, filled on first use.
    bool compute;
    Pass pass;
    bool hdr; // HDR output variant: PQ encoding and brightness constants
    std::array<std::uint8_t, 16> checksum; // DXBC header hash from GlobalShaderCache-PCD3D_SM5.bin.
};

// The upscaling TAA is shared by SDR and HDR; its composite pass differs.
constexpr ShaderSlot Shaders[]{
    {"TBendSMAATemporalAACS_T1X_0000", 0x4CA41C8, true, Pass::TemporalFull, false, dxbcHash("6e855e67ebcbdd2ec77695de2f6121c5")},
    {"TBendSMAATemporalAACS_T1X_0010", 0x4CA41E8, true, Pass::TemporalFull, true, dxbcHash("8ebd31f25f6c9edfd50ae51903a9f2c7")},
    {"TBendSMAATemporalAACS_T1X_0001", 0x4CA4208, true, Pass::TemporalUpscale, false, dxbcHash("2ca645b20725c8f56b8e091fea9eb20a")},
    {"TBendSMAABufferCopyPS_00", 0x4CA41A8, false, Pass::Composite, false, dxbcHash("d406a2061d2c4102c4793a90784656f6")},
    {"TBendSMAABufferCopyPS_01", 0x4CA4198, false, Pass::Composite, false, dxbcHash("92fac8c168f8330fc67c25a8ebb1650d")},
    {"TBendSMAABufferCopyPS_10", 0x4CA4178, false, Pass::Composite, true, dxbcHash("66c0f2b635fc2f0d9e2d86c5e731b13a")},
    {"TBendSMAABufferCopyPS_11", 0x4CA4168, false, Pass::Composite, true, dxbcHash("214506ef0606197b6d402ec92f4aa56b")},
    {"FDepthResolveCS<false>", 0x4C46000, true, Pass::DepthResolve, false, dxbcHash("39b02a2fa20098b2b7aac54c68b4b9cd")},
    {"FDepthResolveCS<true>", 0x4C46010, true, Pass::DepthResolve, false, dxbcHash("3ceef14a17dc9fa2a9c8274ef8601ddd")},
};
constexpr std::size_t NoSlot = std::size(Shaders);

struct PrepareConstants { std::uint32_t renderSize[2], linear, padding; };
struct PixelConstants { std::int32_t outputOrigin[2]; std::uint32_t debugView, hdr; float renderScale[2]; std::uint32_t linear; float sharpness; };
struct ComputeConstants { std::uint32_t outputSize[2], debugView, hdr, linear; float sharpness; std::uint32_t padding[2]; };

struct ViewCapture
{
    std::uint64_t sequence{};
    DWORD thread{};
    std::int32_t rect[4]{}, unscaled[4]{};
    float jitter[2]{};
    bool cameraCut{};
};

struct Target
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
    UINT width{}, height{};
};

struct Plan
{
    UINT renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    NVSDK_NGX_PerfQuality_Value quality{NVSDK_NGX_PerfQuality_Value_DLAA};
    bool operator==(const Plan&) const = default;
};

struct FeatureKey
{
    Plan plan;
    Preset preset{};
    int flags{};
    bool operator==(const FeatureKey&) const = default;
};

// ---- Shared state ----
std::mutex logMutex;
std::ofstream logFile;
std::mutex settingsMutex;
Settings settings;
Diagnostics diagnostics;
std::filesystem::path dataDirectory, settingsPath;
Address imageBase{};

std::atomic<bool> active{}, releaseRequested{}, released{true}, hooked{};
// Published by the render thread when DLSS can run: render width over output width, and jitter phases.
std::atomic<float> targetScale{};
std::atomic<int> targetPhases{};
std::atomic<float> targetMipBias{};
std::atomic<bool> replacing{};

// ---- Game thread ----
float* screenPercentage{};
std::int32_t* temporalSamples{};
struct Applied
{
    bool active{};
    float originalPercentage{}, writtenPercentage{};
    std::int32_t originalSamples{}, writtenSamples{};
} applied;

// ---- Engine hook (render thread) ----
using PreVisibilityFrameSetup = void (*)(Address renderer, Address commandList);
using AllocateCommandPage = void* (*)(Address memory, std::int32_t size);
PreVisibilityFrameSetup originalPreVisibility{};
AllocateCommandPage allocateCommandPage{};
void* preVisibilityAddress{};
std::mutex viewMutex;
// The latest view seen by the render thread, and the one the RHI executor reached in its command list.
ViewCapture capturedView, executingView;
std::uint64_t captureSequence{};

// An RHI command carrying a view capture. The render thread runs ahead of the thread executing
// Direct3D work, so the capture travels in the command list and arrives just before its TAA.
struct ViewCommand
{
    ViewCommand* next;
    void (*execute)(Address list, ViewCommand* command);
    ViewCapture view;
};

// ---- DirectX (render thread) ----
// Set while DLSS issues its own Direct3D calls, so the add-on passes those straight through.
thread_local int bypass{};
struct Bypass
{
    Bypass() { ++bypass; }
    ~Bypass() { --bypass; }
    Bypass(const Bypass&) = delete;
    Bypass& operator=(const Bypass&) = delete;
};

ID3D11DeviceContext* immediate{};
ID3D11PixelShader* currentPixelShader{};
ComPtr<ID3D11Device> device, ngxDevice;
ComPtr<ID3D11Device1> device1;
ComPtr<ID3D11DeviceContext1> context1;
ComPtr<ID3D11DeviceContext> ngxContext;
ComPtr<ID3DDeviceContextState> ownState;
ComPtr<ID3D11ComputeShader> motionShader, prepareShader, compositeComputeShader;
ComPtr<ID3D11PixelShader> compositePixelShader;
ComPtr<ID3D11Buffer> prepareConstants, pixelConstants, computeConstants;
Target motionTarget, depthTarget, biasTarget, colorTarget, outputTarget;
// Identified game shaders: read lock-free on every call, written under the mutex from the render
// thread or whichever thread the game creates shaders on.
// The game can create a shader more than once with the same bytecode, so each slot keeps every
// copy it has seen; the engine cache then shows which one is bound.
constexpr std::size_t CopiesPerShader = 8;
std::mutex shaderMutex;
std::array<std::array<std::atomic<IUnknown*>, CopiesPerShader>, std::size(Shaders)> gameShaders{};
std::array<std::array<ComPtr<IUnknown>, CopiesPerShader>, std::size(Shaders)> shaderOwners;
std::array<std::string, std::size(Shaders)> shaderStatus;
std::array<bool, std::size(Shaders)> foundInCache{};

struct BiasedSampler { ComPtr<ID3D11SamplerState> original, biased; };
std::unordered_map<ID3D11SamplerState*, BiasedSampler> biasedSamplers;
float samplerBias{};

// Bend's own bias for rendering below output resolution (r.Bend.Texture.NeoGlobalBias, PS4 Pro) covers
// only its filtered, wrapping material samplers. Screen-space passes sample with point or clamped
// samplers, and the bias also shifts their explicit mip reads: SSAO would read full-detail depth for
// its wider samples and darken the ground.
bool materialSampler(const D3D11_SAMPLER_DESC& sampler)
{
    const auto tiles = [](D3D11_TEXTURE_ADDRESS_MODE mode)
    {
        return mode == D3D11_TEXTURE_ADDRESS_WRAP || mode == D3D11_TEXTURE_ADDRESS_MIRROR;
    };
    return sampler.MaxLOD > sampler.MinLOD && tiles(sampler.AddressU) && tiles(sampler.AddressV)
        && sampler.Filter != D3D11_FILTER_MIN_MAG_MIP_POINT && !D3D11_DECODE_IS_COMPARISON_FILTER(sampler.Filter);
}

struct Ngx
{
    bool initialized{}, failed{};
    std::string message{"DLSS starts when a mode is selected."};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    FeatureKey key{};
    bool reset{true};
} ngx;

struct Frame
{
    std::uint64_t index{};
    bool motion{}; // Motion and depth were computed at this frame's depth resolve
    bool upscaled{};
    bool linear{}; // DLSS ran in HDR mode on linear colour
    std::int32_t outputOrigin[2]{};
    float renderScale[2]{1, 1};
    float sharpness{};
} frame;
// Whether the game's last composite was the HDR variant; the upscaling TAA does not say.
bool hdrOutput{};
int missedComposites{};

std::optional<Plan> plan;
Settings planSettings;
UINT planOutput[2]{};
std::uint64_t lastEvaluated{};
LARGE_INTEGER lastEvaluation{};
std::uint64_t evaluations{}, fallbacks{};
std::string lastFallback;
std::map<std::string, bool, std::less<>> loggedOnce;
ViewCapture lastUsedView;
bool failed{};
std::string failure;
std::uint64_t dispatchCalls{}, foreignDispatches{}, recognisedDispatches{}, resets{};
ID3D11DeviceContext* lastForeignContext{};

void log(std::string_view message)
{
    std::lock_guard lock(logMutex);
    if (logFile) logFile << GetTickCount64() << " [thread " << GetCurrentThreadId() << "] " << message << std::endl;
}

void logOnce(const std::string& key, std::string_view message)
{
    if (loggedOnce.emplace(key, true).second) log(message);
}

std::string hex(std::uint64_t value)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
}

template<class T> bool safeRead(Address address, T& value)
{
    SIZE_T read{};
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(T), &read)
        && read == sizeof(T);
}

std::filesystem::path moduleDirectory()
{
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleDirectory), &module))
        return {};
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) { path.resize(length); break; }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}

// ---- Engine: jitter and view capture ----

void executeViewCommand(Address, ViewCommand* command)
{
    std::lock_guard lock(viewMutex);
    executingView = command->view;
}

// Mirrors the engine's AllocCommand: append to the last memory page, or let the engine add a page.
void enqueueView(Address list, const ViewCapture& view)
{
    if (!allocateCommandPage || *reinterpret_cast<const bool*>(list + ExecutingOffset)) return;
    constexpr std::int32_t size = sizeof(ViewCommand);
    const auto memory = list + CommandMemoryOffset;
    const auto pages = *reinterpret_cast<const std::int32_t*>(memory + 0x28);
    void* storage{};
    if (pages > 0)
    {
        auto data = *reinterpret_cast<const Address*>(memory + 0x20);
        if (!data) data = memory;
        auto* page = reinterpret_cast<std::int32_t*>(data + static_cast<Address>(pages - 1) * 16);
        const auto offset = (page[0] + 7) & ~7;
        if (page[1] - offset >= size)
        {
            page[0] = offset + size;
            storage = reinterpret_cast<void*>(*reinterpret_cast<const Address*>(page + 2) + static_cast<Address>(offset));
        }
    }
    if (!storage) storage = allocateCommandPage(memory, size);
    auto* command = new (storage) ViewCommand{nullptr, &executeViewCommand, view};
    ++*reinterpret_cast<std::uint32_t*>(list + CommandCountOffset);
    auto& link = *reinterpret_cast<ViewCommand***>(list + CommandLinkOffset);
    *link = command;
    link = &command->next;
}

void preVisibility(Address renderer, Address commandList)
{
    const auto views = *reinterpret_cast<const Address*>(renderer + RendererViews);
    const auto count = views ? std::clamp(*reinterpret_cast<const std::int32_t*>(renderer + RendererViewCount), 0, MaxViews) : 0;
    std::array<float, MaxViews> saved{};
    const bool fullJitter = replacing.load(std::memory_order_relaxed);
    auto temporal = [&](int index)
    {
        const auto view = views + static_cast<Address>(index) * ViewInfoSize;
        return *reinterpret_cast<const std::uint8_t*>(view + AntiAliasingOffset) == BendTemporalAA
            && *reinterpret_cast<const Address*>(view + ViewStateOffset);
    };
    // Below 70, 50 and 30 percent the engine caps TAA jitter at 8, 4 and 2 phases. DLSS needs the
    // full Halton sequence that r.TemporalAASamples asks for, so present full scale for this call.
    if (fullJitter)
        for (int i = 0; i < count; ++i)
            if (temporal(i))
            {
                auto& percentage = *reinterpret_cast<float*>(views + static_cast<Address>(i) * ViewInfoSize + ScreenPercentageOffset);
                saved[i] = percentage;
                percentage = 100.0f;
            }
    originalPreVisibility(renderer, commandList);
    for (int i = 0; i < count; ++i)
    {
        if (!temporal(i)) continue;
        const auto view = views + static_cast<Address>(i) * ViewInfoSize;
        if (fullJitter) *reinterpret_cast<float*>(view + ScreenPercentageOffset) = saved[i];
        ViewCapture capture;
        std::memcpy(capture.rect, reinterpret_cast<const void*>(view + ViewRectOffset), sizeof(capture.rect));
        std::memcpy(capture.unscaled, reinterpret_cast<const void*>(view + UnscaledViewRectOffset), sizeof(capture.unscaled));
        std::memcpy(capture.jitter, reinterpret_cast<const void*>(view + TemporalJitterOffset), sizeof(capture.jitter));
        capture.cameraCut = *reinterpret_cast<const std::uint8_t*>(view + CameraCutOffset) != 0;
        capture.thread = GetCurrentThreadId();
        {
            std::lock_guard lock(viewMutex);
            static std::map<std::array<std::int32_t, 8>, bool> seen;
            const std::array<std::int32_t, 8> key{capture.rect[0], capture.rect[1], capture.rect[2], capture.rect[3],
                capture.unscaled[0], capture.unscaled[1], capture.unscaled[2], capture.unscaled[3]};
            if (seen.size() < 32 && seen.emplace(key, true).second)
                log("View seen: rect " + std::to_string(key[0]) + "," + std::to_string(key[1]) + "-" + std::to_string(key[2]) + ","
                    + std::to_string(key[3]) + " unscaled " + std::to_string(key[4]) + "," + std::to_string(key[5]) + "-"
                    + std::to_string(key[6]) + "," + std::to_string(key[7]) + " view " + std::to_string(i) + "/" + std::to_string(count) + ".");
            capture.sequence = ++captureSequence;
            capturedView = capture;
        }
        if (fullJitter) enqueueView(commandList, capture);
    }
}

ViewCapture latestView()
{
    std::lock_guard lock(viewMutex);
    return capturedView;
}

ViewCapture currentView()
{
    std::lock_guard lock(viewMutex);
    return executingView;
}

// ---- Game thread: render scale and jitter phases ----

void writeConsoleVariables(float percentage, std::int32_t samples)
{
    screenPercentage[0] = screenPercentage[1] = percentage;
    temporalSamples[0] = temporalSamples[1] = samples;
    applied.writtenPercentage = percentage;
    applied.writtenSamples = samples;
}

void restoreConsoleVariables()
{
    if (!applied.active) return;
    // Keep any value the game set meanwhile; only undo what this module wrote.
    if (screenPercentage[0] == applied.writtenPercentage) screenPercentage[0] = screenPercentage[1] = applied.originalPercentage;
    if (temporalSamples[0] == applied.writtenSamples) temporalSamples[0] = temporalSamples[1] = applied.originalSamples;
    applied = {};
    log("Restored r.ScreenPercentage and r.TemporalAASamples.");
}

// ---- DirectX: shader identity ----

bool readable(Address address, std::size_t size)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT
        || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return address + size <= reinterpret_cast<Address>(info.BaseAddress) + info.RegionSize;
}

bool inModule(Address address, HMODULE module)
{
    MODULEINFO info{};
    if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return false;
    const auto base = reinterpret_cast<Address>(info.lpBaseOfDll);
    return address >= base && address < base + info.SizeOfImage;
}

void remember(std::size_t slot, ComPtr<IUnknown> native, std::string status)
{
    std::lock_guard lock(shaderMutex);
    auto& copies = gameShaders[slot];
    std::size_t free = CopiesPerShader;
    for (std::size_t i = 0; i < CopiesPerShader; ++i)
    {
        if (copies[i].load() == native.Get()) return;
        if (!copies[i].load() && free == CopiesPerShader) free = i;
    }
    if (free == CopiesPerShader) return;
    shaderOwners[slot][free] = std::move(native);
    shaderStatus[slot] = std::move(status);
    copies[free] = shaderOwners[slot][free].Get();
    log(std::string("Identified ") + Shaders[slot].name + " (" + shaderStatus[slot] + ") at "
        + hex(reinterpret_cast<Address>(shaderOwners[slot][free].Get())) + ".");
}

// FD3D11ComputeShader/FD3D11PixelShader hold a TRefCountPtr to the native shader and, when the
// engine keeps it, the bytecode. The cached FShaderResource points at the RHI shader at +8.
// This finds shaders created before the add-on could watch shader creation.
void identifyShaders()
{
    const auto runtime = GetModuleHandleW(L"d3d11.dll");
    for (std::size_t slot = 0; slot < std::size(Shaders); ++slot)
    {
        {
            std::lock_guard lock(shaderMutex);
            if (foundInCache[slot] || shaderStatus[slot] == MismatchStatus) continue;
        }
        const auto& shader = Shaders[slot];
        Address resource{}, rhi{};
        if (!safeRead(imageBase + shader.cacheRva, resource) || !resource || !safeRead(resource + 8, rhi) || !rhi
            || !readable(rhi, NativeShaderOffset + 8))
            continue;
        ComPtr<IUnknown> native;
        bool verified{}, mismatch{};
        // Bend's FD3D11 shaders keep the native pointer at +0x98; scan the rest in case a variant differs.
        std::array<std::size_t, 20> offsets{NativeShaderOffset};
        for (std::size_t i = 1; i < offsets.size(); ++i) offsets[i] = i * 8;
        for (const auto offset : offsets)
        {
            Address candidate{}, vtable{};
            if (!safeRead(rhi + offset, candidate) || !readable(candidate, 8) || !safeRead(candidate, vtable)) continue;
            if (!native && inModule(vtable, runtime))
            {
                auto* object = reinterpret_cast<IUnknown*>(candidate);
                const auto& iid = shader.compute ? __uuidof(ID3D11ComputeShader) : __uuidof(ID3D11PixelShader);
                if (SUCCEEDED(object->QueryInterface(iid, reinterpret_cast<void**>(native.GetAddressOf())))) continue;
            }
            // A TArray<uint8> of the bytecode: data pointer followed by its length.
            std::int32_t length{};
            std::array<std::uint8_t, 20> header{};
            if (safeRead(rhi + offset + 8, length) && length > 32 && safeRead(candidate, header)
                && !std::memcmp(header.data(), "DXBC", 4))
            {
                if (std::equal(shader.checksum.begin(), shader.checksum.end(), header.begin() + 4)) verified = true;
                else mismatch = true;
            }
        }
        if (!native) continue;
        if (mismatch && !verified)
        {
            std::lock_guard lock(shaderMutex);
            shaderStatus[slot] = MismatchStatus;
            log(std::string("Shader ") + shader.name + " has unexpected bytecode; DLSS stays off.");
            continue;
        }
        remember(slot, native, verified ? "found in the engine cache, bytecode verified" : "found in the engine cache");
        std::lock_guard lock(shaderMutex);
        foundInCache[slot] = true;
    }
}

std::size_t slotOf(IUnknown* shader)
{
    if (!shader) return NoSlot;
    for (std::size_t slot = 0; slot < std::size(Shaders); ++slot)
        for (const auto& copy : gameShaders[slot])
            if (copy.load(std::memory_order_relaxed) == shader) return slot;
    return NoSlot;
}

Pass classify(IUnknown* shader)
{
    const auto slot = slotOf(shader);
    return slot == NoSlot ? Pass::None : Shaders[slot].pass;
}

bool identified(Pass pass, std::optional<bool> hdr = std::nullopt)
{
    for (std::size_t slot = 0; slot < std::size(Shaders); ++slot)
        if (Shaders[slot].pass == pass && (!hdr || Shaders[slot].hdr == *hdr) && gameShaders[slot][0].load(std::memory_order_relaxed))
            return true;
    return false;
}

// The engine looks shaders up on first use, so the upscale variants only appear after the render
// scale drops. One TAA variant and the depth resolve are enough to start.
bool shadersReady()
{
    return (identified(Pass::TemporalFull) || identified(Pass::TemporalUpscale)) && identified(Pass::DepthResolve);
}

// ---- DirectX: resources ----

ComPtr<ID3DBlob> compile(std::string_view source, const char* name, const char* profile)
{
    std::string text(shaders::Common);
    text += source;
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(text.data(), text.size(), name, nullptr, nullptr, "main", profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error(std::string("Shader ") + name + " did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : hex(static_cast<std::uint32_t>(result))));
    return code;
}

ComPtr<ID3D11Buffer> constantBuffer(UINT size)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = size;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&description, nullptr, buffer.GetAddressOf()), "Creating a DLSS constant buffer");
    return buffer;
}

void createPipeline()
{
    auto code = compile(shaders::Motion, "dlss-motion", "cs_5_0");
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, motionShader.GetAddressOf()), "Creating the DLSS motion shader");
    code = compile(shaders::Prepare, "dlss-prepare", "cs_5_0");
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, prepareShader.GetAddressOf()), "Creating the DLSS input shader");
    code = compile(shaders::CompositeCompute, "dlss-composite-cs", "cs_5_0");
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, compositeComputeShader.GetAddressOf()), "Creating the DLAA composite shader");
    code = compile(shaders::CompositePixel, "dlss-composite-ps", "ps_5_0");
    check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, compositePixelShader.GetAddressOf()), "Creating the DLSS composite shader");
    prepareConstants = constantBuffer(sizeof(PrepareConstants));
    pixelConstants = constantBuffer(sizeof(PixelConstants));
    computeConstants = constantBuffer(sizeof(ComputeConstants));
    // A separate context state lets DLSS run without disturbing the engine's cached bindings.
    const auto level = device->GetFeatureLevel();
    const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    check(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, ownState.GetAddressOf()),
        "Creating the DLSS device context state");
}

void ensureTarget(Target& target, UINT width, UINT height, DXGI_FORMAT format, const char* name)
{
    if (target.texture && target.width == width && target.height == height) return;
    target = {};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateTexture2D(&description, nullptr, target.texture.GetAddressOf()), name);
    check(device->CreateShaderResourceView(target.texture.Get(), nullptr, target.view.GetAddressOf()), name);
    check(device->CreateUnorderedAccessView(target.texture.Get(), nullptr, target.access.GetAddressOf()), name);
    target.width = width;
    target.height = height;
}

// ---- NGX ----

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string text(message ? message : "");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    log("NGX: " + text);
}

void releaseFeature()
{
    if (ngx.feature) NVSDK_NGX_D3D11_ReleaseFeature(ngx.feature);
    ngx.feature = nullptr;
    ngx.key = {};
}

bool initializeNgx()
{
    if (ngx.initialized) return true;
    if (ngx.failed || !ngxDevice) return false;
    const auto modelDirectory = moduleDirectory().wstring();
    const wchar_t* paths[]{modelDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    auto result = NVSDK_NGX_D3D11_Init_with_ProjectID(NgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "4.11-Bend",
        dataDirectory.c_str(), ngxDevice.Get(), &info);
    if (NVSDK_NGX_FAILED(result))
    {
        ngx.failed = true;
        ngx.message = "NVIDIA NGX could not start (" + hex(static_cast<std::uint32_t>(result)) + "). DLSS needs an NVIDIA RTX GPU and a current driver.";
        log(ngx.message);
        return false;
    }
    ngx.initialized = true;
    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&ngx.parameters);
    int available{};
    if (NVSDK_NGX_SUCCEED(result))
        NVSDK_NGX_Parameter_GetI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available)
    {
        int needsDriver{};
        unsigned major{}, minor{};
        int initResult{};
        if (ngx.parameters)
        {
            NVSDK_NGX_Parameter_GetI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            NVSDK_NGX_Parameter_GetUI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
            NVSDK_NGX_Parameter_GetUI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
            NVSDK_NGX_Parameter_GetI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        }
        ngx.failed = true;
        ngx.message = needsDriver
            ? "DLSS needs NVIDIA driver " + std::to_string(major) + "." + std::to_string(minor) + " or newer."
            : "DLSS is unavailable on this GPU (" + hex(static_cast<std::uint32_t>(initResult)) + "). nvngx_dlss.dll must be beside the add-on.";
        log(ngx.message);
        // Shut NGX down so choosing a mode again repeats the whole check.
        if (ngx.parameters) NVSDK_NGX_D3D11_DestroyParameters(ngx.parameters);
        ngx.parameters = nullptr;
        NVSDK_NGX_D3D11_Shutdown1(ngxDevice.Get());
        ngx.initialized = false;
        return false;
    }
    ngx.message = "DLSS ready.";
    log("NGX initialized; DLSS Super Resolution is available. Model directory: " + moduleDirectory().string());
    return true;
}

NVSDK_NGX_PerfQuality_Value perfQuality(Quality quality, float scale)
{
    switch (quality)
    {
    case Quality::Dlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case Quality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case Quality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case Quality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case Quality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default:
        return scale >= 0.99f ? NVSDK_NGX_PerfQuality_Value_DLAA : scale >= 0.66f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
            : scale >= 0.58f ? NVSDK_NGX_PerfQuality_Value_Balanced : scale >= 0.5f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
            : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
}

Plan makePlan(const Settings& wanted, UINT outputWidth, UINT outputHeight)
{
    const auto scale = nominalScale(wanted.quality, wanted.customScale);
    Plan result{0, 0, outputWidth, outputHeight, perfQuality(wanted.quality, scale)};
    if (wanted.quality != Quality::Custom && wanted.quality != Quality::Dlaa)
    {
        UINT maxWidth{}, maxHeight{}, minWidth{}, minHeight{};
        float sharpness{};
        if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_OPTIMAL_SETTINGS(ngx.parameters, outputWidth, outputHeight, result.quality,
            &result.renderWidth, &result.renderHeight, &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness))
            && result.renderWidth && result.renderHeight)
            return result;
    }
    result.renderWidth = std::max(1u, static_cast<UINT>(std::lround(outputWidth * scale)));
    result.renderHeight = std::max(1u, static_cast<UINT>(std::lround(outputHeight * scale)));
    return result;
}

void ensureFeature(ID3D11DeviceContext* context, const Plan& target, const Settings& wanted, int flags)
{
    const FeatureKey key{target, wanted.preset, flags};
    if (ngx.feature && ngx.key == key) return;
    releaseFeature();
    const auto preset = static_cast<unsigned>(wanted.preset);
    for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(ngx.parameters, hint, preset);
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = target.renderWidth;
    create.Feature.InHeight = target.renderHeight;
    create.Feature.InTargetWidth = target.outputWidth;
    create.Feature.InTargetHeight = target.outputHeight;
    create.Feature.InPerfQualityValue = target.quality;
    create.InFeatureCreateFlags = flags;
    const auto result = NGX_D3D11_CREATE_DLSS_EXT(context, &ngx.feature, ngx.parameters, &create);
    if (NVSDK_NGX_FAILED(result))
    {
        ngx.feature = nullptr;
        throw std::runtime_error("Creating the DLSS feature failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
    }
    ngx.key = key;
    ngx.reset = true;
    log("Created DLSS " + std::string(label(wanted.quality)) + ": " + std::to_string(target.renderWidth) + "x"
        + std::to_string(target.renderHeight) + " -> " + std::to_string(target.outputWidth) + "x" + std::to_string(target.outputHeight)
        + ", preset " + std::string(label(wanted.preset)) + ", flags " + hex(static_cast<std::uint32_t>(flags)) + ".");
}

// The SDK's NGX_D3D11_EVALUATE_DLSS_EXT ends in NVSDK_NGX_D3D11_EvaluateFeature_C. Add-ons that
// follow a game's DLSS (RenoDX hooks the non-_C entry point) only see evaluations made through
// NVSDK_NGX_D3D11_EvaluateFeature, so set the same parameters and call that.
NVSDK_NGX_Result evaluateDlss(ID3D11DeviceContext* context, const NVSDK_NGX_D3D11_DLSS_Eval_Params& evaluation)
{
    auto* parameters = ngx.parameters;
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Color, evaluation.Feature.pInColor);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Output, evaluation.Feature.pInOutput);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Depth, evaluation.pInDepth);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_MotionVectors, evaluation.pInMotionVectors);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_TransparencyMask, nullptr);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_ExposureTexture, nullptr);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, evaluation.pInBiasCurrentColorMask);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, evaluation.InJitterOffsetX);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, evaluation.InJitterOffsetY);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Sharpness, 0.0f);
    NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_Reset, evaluation.InReset);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_MV_Scale_X, evaluation.InMVScaleX);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_MV_Scale_Y, evaluation.InMVScaleY);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, evaluation.InFrameTimeDeltaInMsec);
    for (const char* base : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y})
        NVSDK_NGX_Parameter_SetUI(parameters, base, 0);
    NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, evaluation.InRenderSubrectDimensions.Width);
    NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, evaluation.InRenderSubrectDimensions.Height);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    return NVSDK_NGX_D3D11_EvaluateFeature(context, ngx.feature, parameters, nullptr);
}

// ---- Per-frame work ----

bool fallback(std::string reason)
{
    ++fallbacks;
    logOnce("fallback:" + reason, "Using the game's TAA this frame: " + reason);
    lastFallback = std::move(reason);
    return false;
}

void fail(std::string_view reason)
{
    failed = true;
    failure = reason;
    replacing = false;
    targetScale = 0;
    log("DLSS stopped: " + failure);
}

std::string describe(ID3D11ShaderResourceView* view)
{
    if (!view) return "none";
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    view->GetDesc(&viewDescription);
    if (FAILED(resource.As(&texture))) return "non-2D resource";
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return std::to_string(description.Width) + "x" + std::to_string(description.Height) + " texture format "
        + std::to_string(description.Format) + ", view format " + std::to_string(viewDescription.Format);
}

// Runs DLSS's Direct3D work in its own context state, so the engine's cached bindings are untouched.
template<class Work> void inOwnState(ID3D11DeviceContext* context, Work&& work)
{
    Bypass guard;
    ComPtr<ID3DDeviceContextState> gameState;
    context1->SwapDeviceContextState(ownState.Get(), gameState.GetAddressOf());
    const auto restore = [&]
    {
        context->ClearState();
        context1->SwapDeviceContextState(gameState.Get(), nullptr);
    };
    try { work(); }
    catch (...)
    {
        restore();
        throw;
    }
    restore();
}

// FDepthResolveCS packs its motion for the game's TAA in a form DLSS cannot use (shaders::Motion),
// so this repeats the resolve's motion maths on its inputs while they are bound.
void computeMotion(ID3D11DeviceContext* context, UINT groupsX, UINT groupsY)
{
    frame.motion = false;
    if (!plan) return;
    ComPtr<ID3D11Buffer> parameters;
    ComPtr<ID3D11ShaderResourceView> pixels, depth, velocity;
    context->CSGetConstantBuffers(0, 1, parameters.GetAddressOf());
    context->CSGetShaderResources(0, 1, pixels.GetAddressOf());
    context->CSGetShaderResources(1, 1, depth.GetAddressOf());
    context->CSGetShaderResources(3, 1, velocity.GetAddressOf());
    if (!parameters || !pixels || !depth || !velocity) return;
    logOnce("inputs-motion", "Motion inputs: pixel flags " + describe(pixels.Get()) + "; depth " + describe(depth.Get())
        + "; velocity " + describe(velocity.Get()) + ".");

    ensureTarget(motionTarget, plan->renderWidth, plan->renderHeight, DXGI_FORMAT_R16G16_FLOAT, "Creating the DLSS motion texture");
    ensureTarget(depthTarget, plan->renderWidth, plan->renderHeight, DXGI_FORMAT_R32_FLOAT, "Creating the DLSS depth texture");
    ensureTarget(biasTarget, plan->renderWidth, plan->renderHeight, DXGI_FORMAT_R8_UNORM, "Creating the DLSS colour bias texture");
    inOwnState(context, [&]
    {
        ID3D11ShaderResourceView* inputs[]{pixels.Get(), depth.Get(), nullptr, velocity.Get()};
        ID3D11UnorderedAccessView* outputs[]{motionTarget.access.Get(), depthTarget.access.Get(), biasTarget.access.Get()};
        context->CSSetShader(motionShader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, parameters.GetAddressOf());
        context->CSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(outputs)), outputs, nullptr);
        context->Dispatch(groupsX, groupsY, 1);
    });
    frame.motion = true;
}

bool replaceTemporal(ID3D11DeviceContext* context, std::size_t slot, UINT groupsX, UINT groupsY)
{
    const auto pass = Shaders[slot].pass;
    // HDR highlights exceed 1, which DLSS's LDR mode would clip: run it on linear colour instead.
    const bool hdr = pass == Pass::TemporalFull ? Shaders[slot].hdr : hdrOutput;
    const auto wanted = currentSettings();
    const auto options = currentDiagnostics();
    const auto view = currentView();
    if (!plan) return fallback("no DLSS plan yet");
    if (options.gameTemporalAA) return fallback("the diagnostics ask for the game's TAA");
    // Each view command precedes exactly one TAA dispatch; a repeat means the engine skipped one.
    if (!view.sequence || view.sequence == lastUsedView.sequence) return fallback("the engine did not report this frame's view");
    logOnce("threads", "Views are captured on thread " + std::to_string(view.thread) + " and executed on thread "
        + std::to_string(GetCurrentThreadId()) + ".");
    if (view.rect[0] || view.rect[1]) return fallback("the view does not start at the buffer origin");
    const auto renderWidth = static_cast<UINT>(view.rect[2] - view.rect[0]), renderHeight = static_cast<UINT>(view.rect[3] - view.rect[1]);
    const auto outputWidth = static_cast<UINT>(view.unscaled[2] - view.unscaled[0]), outputHeight = static_cast<UINT>(view.unscaled[3] - view.unscaled[1]);
    if (groupsX != (renderWidth + 7) / 8 || groupsY != (renderHeight + 7) / 8) return fallback("the TAA dispatch does not cover the view");
    if (outputWidth != plan->outputWidth || outputHeight != plan->outputHeight) return fallback("the output size changed");
    // The engine rounds the scaled view up; absorb that instead of a frame from the previous mode.
    if (renderWidth > plan->renderWidth && renderWidth * 50 <= plan->renderWidth * 51) plan->renderWidth = renderWidth;
    if (renderHeight > plan->renderHeight && renderHeight * 50 <= plan->renderHeight * 51) plan->renderHeight = renderHeight;
    if (renderWidth > plan->renderWidth || renderHeight > plan->renderHeight || !renderWidth || !renderHeight)
        return fallback("the render size " + std::to_string(renderWidth) + "x" + std::to_string(renderHeight) + " is above the DLSS input size");
    if (pass == Pass::TemporalFull && (renderWidth != outputWidth || renderHeight != outputHeight))
        return fallback("full-resolution TAA with a scaled view");
    // A size absorbed above outgrows the motion texture from the depth resolve, once per mode change.
    if (!frame.motion || motionTarget.width != plan->renderWidth || motionTarget.height != plan->renderHeight)
        return fallback("no motion vectors this frame");
    if (pass == Pass::TemporalUpscale && !identified(Pass::Composite, hdr)) return fallback("the upscale composite pass is not identified yet");

    // The full-resolution TAA also composites the UI into its output, with the constants the replacement reuses.
    ComPtr<ID3D11ShaderResourceView> color, overlay;
    ComPtr<ID3D11Buffer> temporal;
    ComPtr<ID3D11UnorderedAccessView> gameOutput;
    context->CSGetShaderResources(pass == Pass::TemporalFull ? 2 : 1, 1, color.GetAddressOf());
    if (pass == Pass::TemporalFull)
    {
        context->CSGetShaderResources(0, 1, overlay.GetAddressOf());
        context->CSGetConstantBuffers(0, 1, temporal.GetAddressOf());
        context->CSGetUnorderedAccessViews(0, 1, gameOutput.GetAddressOf());
    }
    if (!color || (pass == Pass::TemporalFull && (!overlay || !temporal || !gameOutput)))
        return fallback("the TAA bindings are incomplete");
    logOnce(pass == Pass::TemporalFull ? "inputs-full" : "inputs-upscale", std::string("DLSS inputs (")
        + (pass == Pass::TemporalFull ? "full" : "upscale") + " TAA): colour " + describe(color.Get()) + "; render "
        + std::to_string(renderWidth) + "x" + std::to_string(renderHeight) + " -> " + std::to_string(outputWidth) + "x"
        + std::to_string(outputHeight) + ".");

    ensureTarget(colorTarget, plan->renderWidth, plan->renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the DLSS colour texture");
    ensureTarget(outputTarget, plan->outputWidth, plan->outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the DLSS output texture");

    // The motion vectors are per render pixel and undilated. Without MVLowRes DLSS reads them as
    // dilated output-resolution vectors.
    int flags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (options.jitteredMotion) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);

    inOwnState(context, [&]
    {
        const PrepareConstants prepare{{renderWidth, renderHeight}, hdr, {}};
        context->UpdateSubresource(prepareConstants.Get(), 0, nullptr, &prepare, 0, 0);
        ID3D11ShaderResourceView* prepareInput = color.Get();
        ID3D11UnorderedAccessView* prepareOutput = colorTarget.access.Get();
        context->CSSetShader(prepareShader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, prepareConstants.GetAddressOf());
        context->CSSetShaderResources(0, 1, &prepareInput);
        context->CSSetUnorderedAccessViews(0, 1, &prepareOutput, nullptr);
        context->Dispatch(groupsX, groupsY, 1);
        context->ClearState();

        ensureFeature(ngxContext.Get(), *plan, wanted, flags);
        // Each scene render reports one view; a gap means DLSS missed frames and its history is stale.
        const bool reset = ngx.reset || view.cameraCut || view.sequence != lastUsedView.sequence + 1;
        NVSDK_NGX_D3D11_DLSS_Eval_Params evaluation{};
        evaluation.Feature.pInColor = colorTarget.texture.Get();
        evaluation.Feature.pInOutput = outputTarget.texture.Get();
        evaluation.pInDepth = depthTarget.texture.Get();
        evaluation.pInMotionVectors = motionTarget.texture.Get();
        evaluation.pInBiasCurrentColorMask = biasTarget.texture.Get();
        // Bend adds the jitter to the projection with +Y up; DLSS takes render pixels with +Y down.
        evaluation.InJitterOffsetX = view.jitter[0] * (options.invertJitterX ? -1.0f : 1.0f);
        evaluation.InJitterOffsetY = view.jitter[1] * (options.invertJitterY ? -1.0f : 1.0f);
        evaluation.InRenderSubrectDimensions = {renderWidth, renderHeight};
        evaluation.InReset = reset ? 1 : 0;
        if (reset) ++resets;
        evaluation.InMVScaleX = 1;
        evaluation.InMVScaleY = 1;
        evaluation.InFrameTimeDeltaInMsec = lastEvaluation.QuadPart
            ? static_cast<float>(static_cast<double>(now.QuadPart - lastEvaluation.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)) : 16.7f;
        const auto result = evaluateDlss(ngxContext.Get(), evaluation);
        context->ClearState();
        if (NVSDK_NGX_FAILED(result))
            throw std::runtime_error("DLSS evaluation failed (" + hex(static_cast<std::uint32_t>(result)) + ").");

        if (pass == Pass::TemporalFull)
        {
            const ComputeConstants output{{outputWidth, outputHeight}, static_cast<std::uint32_t>(options.debugView), hdr, hdr,
                wanted.sharpness, {}};
            context->UpdateSubresource(computeConstants.Get(), 0, nullptr, &output, 0, 0);
            ID3D11Buffer* buffers[]{temporal.Get(), computeConstants.Get()};
            ID3D11ShaderResourceView* inputs[]{overlay.Get(), outputTarget.view.Get(), nullptr,
                motionTarget.view.Get(), depthTarget.view.Get(), colorTarget.view.Get(), biasTarget.view.Get()};
            ID3D11UnorderedAccessView* outputs[]{gameOutput.Get()};
            context->CSSetShader(compositeComputeShader.Get(), nullptr, 0);
            context->CSSetConstantBuffers(0, 2, buffers);
            context->CSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
            context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
            context->Dispatch((outputWidth + 7) / 8, (outputHeight + 7) / 8, 1);
        }
        ngx.reset = false;
    });

    lastEvaluated = frame.index;
    lastEvaluation = now;
    lastUsedView = view;
    ++evaluations;
    if (evaluations == 1) log("First DLSS frame evaluated.");
    frame.upscaled = pass == Pass::TemporalUpscale;
    frame.linear = hdr;
    frame.outputOrigin[0] = view.unscaled[0];
    frame.outputOrigin[1] = view.unscaled[1];
    frame.renderScale[0] = static_cast<float>(renderWidth) / static_cast<float>(outputWidth);
    frame.renderScale[1] = static_cast<float>(renderHeight) / static_cast<float>(outputHeight);
    frame.sharpness = wanted.sharpness;
    return true;
}

// Draws the game's CopyTemporalAA pass with the DLSS output in place of its TAA history.
template<class Call> void composite(ID3D11DeviceContext* context, Call&& call)
{
    Bypass guard;
    const auto options = currentDiagnostics();
    // Slots t1..t6: the game binds its history at t1 and an auxiliary history at t2.
    std::array<ID3D11ShaderResourceView*, 6> gameInputs{};
    ID3D11Buffer* gameConstants{};
    ID3D11PixelShader* gamePixelShader = currentPixelShader;
    const auto slot = slotOf(gamePixelShader);
    const bool hdr = slot != NoSlot && Shaders[slot].hdr;
    if (hdr != hdrOutput) log(std::string("The game switched to ") + (hdr ? "HDR" : "SDR") + " output.");
    hdrOutput = hdr;
    context->PSGetShaderResources(1, static_cast<UINT>(gameInputs.size()), gameInputs.data());
    context->PSGetConstantBuffers(1, 1, &gameConstants);
    const PixelConstants constants{{frame.outputOrigin[0], frame.outputOrigin[1]}, static_cast<std::uint32_t>(options.debugView), hdr,
        {frame.renderScale[0], frame.renderScale[1]}, frame.linear, frame.sharpness};
    context->UpdateSubresource(pixelConstants.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11ShaderResourceView* ours[]{outputTarget.view.Get(), gameInputs[1], motionTarget.view.Get(), depthTarget.view.Get(),
        colorTarget.view.Get(), biasTarget.view.Get()};
    ID3D11Buffer* buffer = pixelConstants.Get();
    context->PSSetShaderResources(1, static_cast<UINT>(std::size(ours)), ours);
    context->PSSetConstantBuffers(1, 1, &buffer);
    context->PSSetShader(compositePixelShader.Get(), nullptr, 0);
    call();
    // Restore exactly what the engine's state cache believes is bound.
    context->PSSetShader(gamePixelShader, nullptr, 0);
    context->PSSetShaderResources(1, static_cast<UINT>(gameInputs.size()), gameInputs.data());
    context->PSSetConstantBuffers(1, 1, &gameConstants);
    for (auto* input : gameInputs)
        if (input) input->Release();
    if (gameConstants) gameConstants->Release();
    frame.upscaled = false;
}

bool compositing(ID3D11DeviceContext* context)
{
    if (bypass || context != immediate || !frame.upscaled) return false;
    return classify(currentPixelShader) == Pass::Composite;
}

void releaseRenderer()
{
    replacing = false;
    releaseFeature();
    if (ngx.parameters) NVSDK_NGX_D3D11_DestroyParameters(ngx.parameters);
    ngx.parameters = nullptr;
    if (ngx.initialized && ngxDevice) NVSDK_NGX_D3D11_Shutdown1(ngxDevice.Get());
    ngx.initialized = false;
    frame = {};
    biasedSamplers.clear();
    motionTarget = depthTarget = biasTarget = colorTarget = outputTarget = {};
    {
        std::lock_guard lock(shaderMutex);
        for (std::size_t slot = 0; slot < std::size(Shaders); ++slot)
        {
            for (std::size_t i = 0; i < CopiesPerShader; ++i)
            {
                gameShaders[slot][i] = nullptr;
                shaderOwners[slot][i].Reset();
            }
            shaderStatus[slot].clear();
            foundInCache[slot] = false;
        }
    }
    currentPixelShader = nullptr;
    immediate = nullptr;
    motionShader.Reset();
    prepareShader.Reset();
    compositeComputeShader.Reset();
    compositePixelShader.Reset();
    prepareConstants.Reset();
    pixelConstants.Reset();
    computeConstants.Reset();
    ownState.Reset();
    context1.Reset();
    ngxContext.Reset();
    ngxDevice.Reset();
    device1.Reset();
    device.Reset();
    hooked = false;
    log("Released DLSS resources.");
}

// Decides whether DLSS can run and what the game thread should render at.
void updatePlan(const Settings& wanted)
{
    const auto view = latestView();
    const auto outputWidth = static_cast<UINT>(std::max(0, view.unscaled[2] - view.unscaled[0]));
    const auto outputHeight = static_cast<UINT>(std::max(0, view.unscaled[3] - view.unscaled[1]));
    if (wanted.quality == Quality::Off || failed || !outputWidth || !outputHeight || !initializeNgx() || !shadersReady())
    {
        replacing = false;
        targetScale = 0;
        targetMipBias = 0;
        if (wanted.quality == Quality::Off && ngx.feature) releaseFeature();
        plan.reset();
        return;
    }
    // Only the mode and custom scale decide the render size; a new plan would also undo the rounding
    // absorbed in replaceTemporal and recreate the feature while a slider is dragged.
    if (!plan || planSettings.quality != wanted.quality || planSettings.customScale != wanted.customScale
        || planOutput[0] != outputWidth || planOutput[1] != outputHeight)
    {
        plan = makePlan(wanted, outputWidth, outputHeight);
        planSettings = wanted;
        planOutput[0] = outputWidth;
        planOutput[1] = outputHeight;
        log("DLSS " + std::string(label(wanted.quality)) + " plan: render " + std::to_string(plan->renderWidth) + "x"
            + std::to_string(plan->renderHeight) + " for " + std::to_string(outputWidth) + "x" + std::to_string(outputHeight) + ".");
    }
    const auto scale = static_cast<float>(plan->renderWidth) / static_cast<float>(plan->outputWidth);
    targetScale = scale;
    targetPhases = jitterPhases(scale);
    targetMipBias = mipBias(scale, wanted.mipBiasOffset);
    replacing = true;
}
}


Settings currentSettings()
{
    std::lock_guard lock(settingsMutex);
    return settings;
}

Diagnostics currentDiagnostics()
{
    std::lock_guard lock(settingsMutex);
    return diagnostics;
}

void applySettings(const Settings& wanted)
{
    {
        std::lock_guard lock(settingsMutex);
        settings = wanted;
    }
    // Choosing a mode again retries after a failure.
    failed = false;
    ngx.failed = false;
    try { saveSettings(settingsPath, wanted); }
    catch (const std::exception& error) { log("Could not save upscaler settings: " + std::string(error.what())); }
}

void applyDiagnostics(const Diagnostics& options)
{
    std::lock_guard lock(settingsMutex);
    diagnostics = options;
}

void initialize(std::uintptr_t base, const std::filesystem::path& directory)
{
    {
        std::lock_guard lock(logMutex);
        logFile.open(directory / "DaysGoneDLSS.log", std::ios::app);
    }
    imageBase = base;
    dataDirectory = directory;
    settingsPath = directory / "DaysGoneDLSS.ini";
    try
    {
        std::lock_guard lock(settingsMutex);
        settings = loadSettings(settingsPath);
    }
    catch (const std::exception& error) { log("Could not load upscaler settings: " + std::string(error.what())); }

    Address screenData{}, samplesData{};
    float percentage{};
    std::int32_t samples{};
    if (!safeRead(base + ScreenPercentageDataRva, screenData) || !safeRead(screenData, percentage)
        || !safeRead(base + TemporalSamplesDataRva, samplesData) || !safeRead(samplesData, samples)
        || !std::isfinite(percentage) || percentage <= 0 || percentage > 400 || samples < 0 || samples > 255)
        throw std::runtime_error("r.ScreenPercentage or r.TemporalAASamples is not where build 19221447 keeps it.");
    screenPercentage = reinterpret_cast<float*>(screenData);
    temporalSamples = reinterpret_cast<std::int32_t*>(samplesData);

    const auto allocator = reinterpret_cast<const void*>(base + AllocateCommandPageRva);
    if (std::memcmp(allocator, AllocateCommandPagePrefix, sizeof(AllocateCommandPagePrefix)))
        throw std::runtime_error("The RHI command allocator does not match build 19221447.");
    allocateCommandPage = reinterpret_cast<AllocateCommandPage>(base + AllocateCommandPageRva);
    preVisibilityAddress = reinterpret_cast<void*>(base + PreVisibilityFrameSetupRva);
    if (std::memcmp(preVisibilityAddress, PreVisibilityPrefix, sizeof(PreVisibilityPrefix)))
        throw std::runtime_error("FSceneRenderer::PreVisibilityFrameSetup does not match build 19221447.");
    if (const auto status = MH_CreateHook(preVisibilityAddress, reinterpret_cast<void*>(&preVisibility),
        reinterpret_cast<void**>(&originalPreVisibility)); status != MH_OK)
        throw std::runtime_error(std::string("Hooking PreVisibilityFrameSetup: ") + MH_StatusToString(status));
    if (const auto status = MH_EnableHook(preVisibilityAddress); status != MH_OK)
    {
        MH_RemoveHook(preVisibilityAddress);
        throw std::runtime_error(std::string("Enabling the view hook: ") + MH_StatusToString(status));
    }
    releaseRequested = false;
    active = true;
    log("Upscaler ready: r.ScreenPercentage " + std::to_string(percentage) + ", r.TemporalAASamples "
        + std::to_string(samples) + ", mode " + std::string(label(currentSettings().quality)) + ".");
}

void tick() noexcept
{
    if (!active || !screenPercentage) return;
    const auto scale = targetScale.load();
    if (scale <= 0)
    {
        restoreConsoleVariables();
        return;
    }
    const auto percentage = scale * 100.0f;
    const auto samples = static_cast<std::int32_t>(targetPhases.load());
    if (!applied.active)
    {
        applied.active = true;
        applied.originalPercentage = screenPercentage[0];
        applied.originalSamples = temporalSamples[0];
        log("Overriding r.ScreenPercentage " + std::to_string(applied.originalPercentage) + " -> " + std::to_string(percentage)
            + " and r.TemporalAASamples " + std::to_string(applied.originalSamples) + " -> " + std::to_string(samples) + ".");
    }
    else
    {
        // The game's Render Scale option writes r.ScreenPercentage; remember it for when DLSS is turned off.
        if (screenPercentage[0] != applied.writtenPercentage) applied.originalPercentage = screenPercentage[0];
        if (temporalSamples[0] != applied.writtenSamples) applied.originalSamples = temporalSamples[0];
    }
    if (screenPercentage[0] != percentage || screenPercentage[1] != percentage || temporalSamples[0] != samples || temporalSamples[1] != samples)
        writeConsoleVariables(percentage, samples);
}

void disable() noexcept
{
    if (!active.exchange(false)) return;
    replacing = false;
    targetScale = 0;
    targetMipBias = 0;
    restoreConsoleVariables();
    releaseRequested = true;
    log("Upscaler disabled.");
}

void resume() noexcept
{
    if (!preVisibilityAddress || active) return;
    releaseRequested = false;
    active = true;
    log("Upscaler resumed.");
}

void shutdown() noexcept
{
    disable();
    // The render thread releases NGX at its next frame end; a paused game may never present.
    for (int i = 0; i < 40 && hooked && !released; ++i) Sleep(50);
    if (hooked && !released) log("The render thread did not present; DLSS resources were not released.");
    if (preVisibilityAddress) MH_DisableHook(preVisibilityAddress);
    preVisibilityAddress = nullptr;
    log("Upscaler engine hook removed.");
}

bool wanted() noexcept
{
    try { return active && !failed && currentSettings().quality != Quality::Off; }
    catch (...) { return false; }
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* context, ID3D11DeviceContext* wrapped) noexcept
{
    try
    {
        if (hooked) return;
        device = gameDevice;
        check(device.As(&device1), "Finding Direct3D 11.1");
        ComPtr<ID3D11DeviceContext> native(context);
        check(native.As(&context1), "Finding the Direct3D 11.1 context");
        ngxContext = wrapped ? wrapped : context;
        ngxContext->GetDevice(ngxDevice.GetAddressOf());
        createPipeline();
        immediate = context;
        hooked = true;
        released = false;
        log("Attached to the immediate context " + hex(reinterpret_cast<Address>(context)) + "; NGX uses "
            + (wrapped && wrapped != context ? "ReShade's wrapper " + hex(reinterpret_cast<Address>(wrapped)) : std::string("it directly")) + ".");
    }
    catch (const std::exception& error)
    {
        device.Reset();
        device1.Reset();
        context1.Reset();
        ngxContext.Reset();
        ngxDevice.Reset();
        fail(error.what());
    }
}

void detach() noexcept
{
    try
    {
        if (hooked) releaseRenderer();
        released = true;
    }
    catch (...) {}
}

bool attached() noexcept { return hooked; }

bool dispatch(ID3D11DeviceContext* context, unsigned x, unsigned y, unsigned z) noexcept
{
    ++dispatchCalls;
    if (context != immediate) { ++foreignDispatches; lastForeignContext = context; }
    if (bypass || context != immediate || !replacing.load(std::memory_order_relaxed)) return false;
    try
    {
        ComPtr<ID3D11ComputeShader> shader;
        context->CSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        const auto slot = slotOf(shader.Get());
        const auto pass = slot == NoSlot ? Pass::None : Shaders[slot].pass;
        if (pass != Pass::None) ++recognisedDispatches;
        if (pass == Pass::DepthResolve) computeMotion(context, x, y);
        else if ((pass == Pass::TemporalFull || pass == Pass::TemporalUpscale) && z == 1)
        {
            if (pass == Pass::TemporalFull) hdrOutput = Shaders[slot].hdr;
            return replaceTemporal(context, slot, x, y);
        }
    }
    catch (const std::exception& error) { fail(error.what()); }
    return false;
}

bool draw(ID3D11DeviceContext* context, unsigned count, unsigned start, int base, bool indexed) noexcept
{
    if (!compositing(context)) return false;
    try
    {
        composite(context, [&]
        {
            if (indexed) context->DrawIndexed(count, start, base);
            else context->Draw(count, start);
        });
        return true;
    }
    catch (const std::exception& error) { fail(error.what()); }
    return false;
}

void pixelShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader) noexcept
{
    if (context == immediate && !bypass) currentPixelShader = shader;
}

// DLSS renders below output resolution, so mipmapped textures need the matching negative LOD bias.
// Material samplers are created at load time; the add-on binds biased copies in their place.
bool biasSamplers(ID3D11DeviceContext* context, unsigned count, ID3D11SamplerState* const* samplers,
    ID3D11SamplerState** biased) noexcept
{
    const auto bias = targetMipBias.load(std::memory_order_relaxed);
    if (bypass || context != immediate || bias == 0 || !device || !samplers || count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return false;
    try
    {
        if (bias != samplerBias)
        {
            biasedSamplers.clear();
            samplerBias = bias;
        }
        bool replaced{};
        for (unsigned i = 0; i < count; ++i)
        {
            biased[i] = samplers[i];
            if (!samplers[i]) continue;
            auto found = biasedSamplers.find(samplers[i]);
            if (found == biasedSamplers.end())
            {
                BiasedSampler entry{samplers[i], nullptr};
                D3D11_SAMPLER_DESC description{};
                samplers[i]->GetDesc(&description);
                if (materialSampler(description))
                {
                    description.MipLODBias = std::clamp(description.MipLODBias + bias, D3D11_MIP_LOD_BIAS_MIN, D3D11_MIP_LOD_BIAS_MAX);
                    device->CreateSamplerState(&description, entry.biased.GetAddressOf());
                }
                found = biasedSamplers.emplace(samplers[i], std::move(entry)).first;
            }
            if (found->second.biased)
            {
                biased[i] = found->second.biased.Get();
                replaced = true;
            }
        }
        return replaced;
    }
    catch (...) { return false; }
}

void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    if (!code || size < 20 || !shader || std::memcmp(code, "DXBC", 4)) return;
    const auto* bytes = static_cast<const std::uint8_t*>(code);
    for (std::size_t slot = 0; slot < std::size(Shaders); ++slot)
        if (std::equal(Shaders[slot].checksum.begin(), Shaders[slot].checksum.end(), bytes + 4))
        {
            try { remember(slot, ComPtr<IUnknown>(shader), "created, bytecode verified"); }
            catch (...) {}
        }
}

// Developer override: DaysGoneDLSS-diagnostics.ini beside the settings is re-read when it changes, so
// jitter and motion conventions can be compared on a running game without the overlay.
static void reloadDiagnostics()
{
    static std::filesystem::file_time_type seen{};
    std::error_code error;
    const auto path = dataDirectory / "DaysGoneDLSS-diagnostics.ini";
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error || modified == seen) return;
    seen = modified;
    std::ifstream input(path);
    auto options = currentDiagnostics();
    std::string line;
    while (std::getline(input, line))
    {
        const auto separator = line.find('=');
        if (separator == line.npos) continue;
        const auto key = line.substr(0, separator);
        const auto value = std::atoi(line.c_str() + separator + 1);
        if (key == "invert_jitter_x") options.invertJitterX = value != 0;
        else if (key == "invert_jitter_y") options.invertJitterY = value != 0;
        else if (key == "jittered_motion") options.jitteredMotion = value != 0;
        else if (key == "debug_view" && value >= 0 && value <= 4) options.debugView = value;
        else if (key == "game_taa") options.gameTemporalAA = value != 0;
    }
    applyDiagnostics(options);
    log("Diagnostics: invert jitter X " + std::to_string(options.invertJitterX) + ", Y " + std::to_string(options.invertJitterY)
        + ", jittered motion " + std::to_string(options.jitteredMotion) + ", debug view " + std::to_string(options.debugView) + ".");
}

void endFrame() noexcept
{
    try
    {
        if (releaseRequested)
        {
            if (!released.exchange(true) && hooked) releaseRenderer();
            return;
        }
        if (!active || !hooked) return;
        if (frame.index % 30 == 0) reloadDiagnostics();
        const auto wanted = currentSettings();
        // Missing slots cost one memory read each until the engine fills its caches.
        identifyShaders();
        // Without the composite the game would show its stale TAA history; stop rather than guess.
        // A switch between SDR and HDR output can cost one frame; repeated misses mean an unknown pass.
        if (frame.upscaled)
        {
            log("The upscale composite pass did not run after DLSS; the game showed its own history this frame.");
            if (++missedComposites >= 3) fail("the game's upscale composite pass did not run after DLSS.");
        }
        else missedComposites = 0;
        updatePlan(wanted);
        if (replacing && frame.index % 600 == 0)
            log("Status: frame " + std::to_string(frame.index) + ", DLSS frames " + std::to_string(evaluations)
                + ", game TAA frames " + std::to_string(fallbacks) + ", last fallback: " + (lastFallback.empty() ? "none" : lastFallback)
                + ", jitter " + std::to_string(lastUsedView.jitter[0]) + ", " + std::to_string(lastUsedView.jitter[1])
                + "; dispatches " + std::to_string(dispatchCalls) + ", other contexts " + std::to_string(foreignDispatches)
                + " (last " + hex(reinterpret_cast<Address>(lastForeignContext)) + "), recognised " + std::to_string(recognisedDispatches)
                + ", history resets " + std::to_string(resets) + ".");
        const auto next = frame.index + 1;
        frame = {};
        frame.index = next;
    }
    catch (const std::exception& error) { fail(error.what()); }
}

Status status()
{
    Status result;
    result.active = active;
    result.failed = failed;
    result.failure = failure;
    result.ngxFailed = ngx.failed;
    result.ngxMessage = ngx.message;
    result.shadersReady = shadersReady();
    result.planned = plan.has_value();
    if (plan)
    {
        result.renderWidth = plan->renderWidth;
        result.renderHeight = plan->renderHeight;
        result.outputWidth = plan->outputWidth;
        result.outputHeight = plan->outputHeight;
    }
    result.running = evaluations && lastEvaluated + 2 >= frame.index;
    result.wrappedContext = ngxContext && ngxContext.Get() != immediate;
    result.lastFallback = lastFallback;
    result.evaluations = evaluations;
    result.fallbacks = fallbacks;
    result.jitter[0] = lastUsedView.jitter[0];
    result.jitter[1] = lastUsedView.jitter[1];
    result.viewRender[0] = lastUsedView.rect[2] - lastUsedView.rect[0];
    result.viewRender[1] = lastUsedView.rect[3] - lastUsedView.rect[1];
    result.viewOutput[0] = lastUsedView.unscaled[2] - lastUsedView.unscaled[0];
    result.viewOutput[1] = lastUsedView.unscaled[3] - lastUsedView.unscaled[1];
    result.cameraCut = lastUsedView.cameraCut;
    result.mipBias = targetMipBias.load();
    std::lock_guard lock(shaderMutex);
    for (std::size_t slot = 0; slot < std::size(Shaders); ++slot)
        result.shaders[slot] = std::string(Shaders[slot].name) + ": " + (shaderStatus[slot].empty() ? "not seen yet" : shaderStatus[slot]);
    return result;
}
}
