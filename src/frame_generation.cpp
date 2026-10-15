// DLSS Frame Generation through Streamline; frame_generation.hpp describes the route.
#include "frame_generation.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <reshade.hpp>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>

namespace days_gone_dlss::frame_generation
{
namespace
{
// The NGX project the upscaler's DLSS runs under (upscaler.cpp).
constexpr char ProjectId[] = "8d3b6f2e-5c1a-4e7b-9f0d-2a6c4b8e1d73";
const sl::ViewportHandle Viewport{0u};
// UE4's projection puts the far plane at infinity; Streamline wants a distance. 10 km, in centimetres.
constexpr float FarPlane = 1.0e6f;

// The Streamline functions the add-on calls, from sl.interposer.dll and the plugins it loads.
struct Api
{
    HMODULE module{};
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slIsFeatureSupported* isFeatureSupported{};
    PFun_slSetD3DDevice* setDevice{};
    PFun_slUpgradeInterface* upgradeInterface{};
    PFun_slGetNativeInterface* getNativeInterface{};
    PFun_slGetFeatureFunction* getFeatureFunction{};
    PFun_slGetNewFrameToken* getFrameToken{};
    PFun_slSetTagForFrame* setTags{};
    PFun_slSetConstants* setConstants{};
    PFun_slDLSSGSetOptions* setOptions{};
    PFun_slDLSSGGetState* getState{};
    PFun_slReflexSetOptions* setReflexOptions{};
    PFun_slReflexGetState* getReflexState{};
    PFun_slReflexSleep* sleep{};
    PFun_slPCLSetMarker* setMarker{};
} api;

bool initialized{};
// Read by the game and render threads for their markers.
std::atomic<bool> attached{};
// The game thread marks the simulation; otherwise the presenting thread does.
std::atomic<bool> simulationMarked{};

// What the settings ask for, what the GPU allows and what Streamline was last told.
std::atomic<unsigned> wantedMultiplier{2};
std::atomic<bool> wantedDynamic{};
std::atomic<unsigned> maxMultiplier{};
std::atomic<bool> dynamicSupported{};
struct Mode
{
    bool on{}, dynamic{};
    unsigned generatedFrames{1};

    bool operator==(const Mode&) const = default;
} applied;

// The previous frame's camera, for the clip-to-previous-clip transform.
struct PreviousCamera
{
    bool known{};
    std::uint32_t frame{};
    sl::float4x4 viewToWorld{}, viewToClip{};
} previous;

std::mutex statusMutex;
std::string statusText{"Off."};
std::uint64_t presented{};

void setStatus(std::string text)
{
    std::lock_guard lock(statusMutex);
    statusText = std::move(text);
}

void check(sl::Result result, const char* what)
{
    if (result != sl::Result::eOk) throw std::runtime_error(std::string(what) + " failed (" + sl::getResultAsStr(result) + ").");
}

// Logs a failure once per call site; per-frame calls would otherwise flood the log.
void report(sl::Result result, const char* what)
{
    if (result == sl::Result::eOk) return;

    static std::mutex loggedMutex;
    static std::string logged;
    std::lock_guard lock(loggedMutex);
    if (logged.find(what) != std::string::npos) return;
    logged += what;
    reshade::log::message(reshade::log::level::warning,
        (std::string("Days Gone DLSS: ") + what + " failed (" + sl::getResultAsStr(result) + ").").c_str());
}

// Streamline's warnings and errors also go to ReShade.log, up to a limit: some repeat every frame.
// sl.log beside the add-on keeps all of them.
void logMessage(sl::LogType type, const char* message)
{
    static std::atomic<int> forwarded{};
    if (type == sl::LogType::eInfo || ++forwarded > 50) return;
    std::string text = std::string("Days Gone DLSS: Streamline: ") + message;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    reshade::log::message(type == sl::LogType::eError ? reshade::log::level::error : reshade::log::level::warning, text.c_str());
}

template<class Function> void resolve(Function*& function, const char* name)
{
    function = reinterpret_cast<Function*>(GetProcAddress(api.module, name));
    if (!function) throw std::runtime_error(std::string("sl.interposer.dll lacks ") + name + ".");
}

template<class Function> void resolve(sl::Feature feature, Function*& function, const char* name)
{
    void* address{};
    check(api.getFeatureFunction(feature, name, address), name);
    function = reinterpret_cast<Function*>(address);
}

sl::FrameToken* token(std::uint32_t frame)
{
    sl::FrameToken* result{};
    return api.getFrameToken(result, &frame) == sl::Result::eOk ? result : nullptr;
}

sl::float4x4 matrix(const float (&values)[16])
{
    sl::float4x4 result;
    for (int row = 0; row < 4; ++row)
        result[row] = sl::float4(values[row * 4], values[row * 4 + 1], values[row * 4 + 2], values[row * 4 + 3]);
    return result;
}

sl::float4x4 identity()
{
    return {sl::float4(1, 0, 0, 0), sl::float4(0, 1, 0, 0), sl::float4(0, 0, 1, 0), sl::float4(0, 0, 0, 1)};
}

// Streamline's camera from the engine's: both use row vectors, and UE4's view space has x right,
// y up and z forward, as Streamline's does.
sl::Constants constants(std::uint32_t frame, const Inputs& inputs)
{
    const auto& camera = inputs.camera;
    auto viewToClip = matrix(camera.projection);
    viewToClip[2].x -= camera.projectionJitter[0];
    viewToClip[2].y -= camera.projectionJitter[1];
    // The translated view's rotation is the camera's; its position in the world is the view origin.
    sl::float4x4 viewToWorld;
    sl::matrixOrthoNormalInvert(viewToWorld, matrix(camera.translatedView));
    viewToWorld[3] = sl::float4(camera.origin[0], camera.origin[1], camera.origin[2], 1);

    const bool reset = inputs.reset || !previous.known || frame != previous.frame + 1;
    if (reset)
    {
        previous.viewToWorld = viewToWorld;
        previous.viewToClip = viewToClip;
    }
    sl::Constants values{};
    values.cameraViewToClip = viewToClip;
    sl::matrixFullInvert(values.clipToCameraView, viewToClip);
    values.clipToLensClip = identity();
    sl::float4x4 viewToPreviousView, clipToPreviousView;
    sl::calcCameraToPrevCamera(viewToPreviousView, viewToWorld, previous.viewToWorld);
    sl::matrixMul(clipToPreviousView, values.clipToCameraView, viewToPreviousView);
    sl::matrixMul(values.clipToPrevClip, clipToPreviousView, previous.viewToClip);
    sl::matrixFullInvert(values.prevClipToClip, values.clipToPrevClip);
    values.jitterOffset = sl::float2(inputs.jitter[0], inputs.jitter[1]);
    values.mvecScale = sl::float2(1.0f / static_cast<float>(inputs.renderWidth), 1.0f / static_cast<float>(inputs.renderHeight));
    values.cameraPinholeOffset = sl::float2(0, 0);
    values.cameraPos = sl::float3(camera.origin[0], camera.origin[1], camera.origin[2]);
    values.cameraRight = sl::float3(viewToWorld[0].x, viewToWorld[0].y, viewToWorld[0].z);
    values.cameraUp = sl::float3(viewToWorld[1].x, viewToWorld[1].y, viewToWorld[1].z);
    values.cameraFwd = sl::float3(viewToWorld[2].x, viewToWorld[2].y, viewToWorld[2].z);
    // Reversed infinite projection: row 3's z is the near plane.
    values.cameraNear = viewToClip[3].z;
    values.cameraFar = FarPlane;
    values.cameraFOV = 2 * std::atan(1 / viewToClip[1].y);
    values.cameraAspectRatio = viewToClip[1].y / viewToClip[0].x;
    values.depthInverted = sl::Boolean::eTrue;
    values.cameraMotionIncluded = sl::Boolean::eTrue;
    values.motionVectors3D = sl::Boolean::eFalse;
    values.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    values.motionVectorsJittered = inputs.jitteredMotion ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    previous = {true, frame, viewToWorld, viewToClip};
    return values;
}

// Tells Streamline what to generate when the settings or the frame's inputs change it.
void applyMode(bool valid)
{
    const auto multiplier = wantedMultiplier.load();
    const auto most = maxMultiplier.load();
    Mode wanted;
    wanted.dynamic = wantedDynamic;
    wanted.on = valid && (wanted.dynamic || multiplier >= 2);
    if (!wanted.dynamic && multiplier >= 2) wanted.generatedFrames = (most ? std::min(multiplier, most) : multiplier) - 1;
    if (!wanted.on) wanted = {};
    if (wanted == applied) return;

    sl::DLSSGOptions options{};
    options.mode = !wanted.on ? sl::DLSSGMode::eOff : wanted.dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
    options.numFramesToGenerate = wanted.generatedFrames;
    // Frames without DLSS (menus, loading) turn generation off briefly; keep its memory meanwhile.
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    // Dynamic generation follows the display's refresh rate.
    options.dynamicTargetFrameRate = 0;
    report(api.setOptions(Viewport, options), "Setting the frame generation mode");
    applied = wanted;
}

void marker(sl::PCLMarker which, std::uint32_t frame)
{
    if (auto* frameToken = token(frame)) report(api.setMarker(which, *frameToken), "Setting a PC Latency marker");
}

// Reflex's report: from the tick's start to the GPU finishing the frame, averaged over its recent
// frames. A frame is complete only when its markers on every thread carry the same number.
std::string latency()
{
    static sl::ReflexState state;
    if (api.getReflexState(state) != sl::Result::eOk || !state.latencyReportAvailable) return " Reflex reports no latency.";
    std::uint64_t total{}, frames{};
    for (const auto& entry : state.frameReport)
        if (entry.simStartTime && entry.gpuRenderEndTime > entry.simStartTime)
        {
            total += entry.gpuRenderEndTime - entry.simStartTime;
            ++frames;
        }
    if (!frames) return " Reflex has no complete frames.";
    return " Latency " + std::to_string((total / frames + 500) / 1000) + " ms.";
}

void updateStatus()
{
    if (++presented % 30) return;

    sl::DLSSGState state{};
    if (api.getState(Viewport, state, nullptr) != sl::Result::eOk) return setStatus("Streamline does not report its state.");
    maxMultiplier = state.numFramesToGenerateMax + 1;
    dynamicSupported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    if (state.status != sl::DLSSGStatus::eOk)
    {
        std::string reasons;
        const auto add = [&](sl::DLSSGStatus flag, const char* reason)
        {
            if (!(state.status & flag)) return;
            if (!reasons.empty()) reasons += "; ";
            reasons += reason;
        };
        add(sl::DLSSGStatus::eFailResolutionTooLow, "the resolution is too low");
        add(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime, "Reflex is not detected");
        add(sl::DLSSGStatus::eFailHDRFormatNotSupported, "the HDR format is not supported");
        add(sl::DLSSGStatus::eFailCommonConstantsInvalid, "the camera constants are invalid");
        add(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled, "the back buffer index is not queried");
        return setStatus("Not generating: " + (reasons.empty() ? std::string("unknown reason") : reasons) + ".");
    }
    if (!wantedDynamic && wantedMultiplier < 2) return setStatus("Off." + latency());
    if (!applied.on) return setStatus("Paused: DLSS or DLAA is not running on this frame." + latency());
    setStatus(std::string(applied.dynamic ? "Generating dynamically: " : "Generating: ")
        + std::to_string(state.numFramesActuallyPresented) + " frames presented per rendered frame." + latency());
}
}

bool start(const std::filesystem::path& directory, IDXGIAdapter* adapter) noexcept
{
    try
    {
        const auto interposer = directory / L"sl.interposer.dll";
        if (!std::filesystem::exists(interposer)) throw std::runtime_error("sl.interposer.dll is missing beside the add-on.");
        // Streamline loads only plugins NVIDIA signed; its loader is checked the same way.
        if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) throw std::runtime_error("sl.interposer.dll is not signed by NVIDIA.");
        api.module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!api.module) throw std::runtime_error("Loading sl.interposer.dll failed.");
        resolve(api.init, "slInit");
        resolve(api.shutdown, "slShutdown");
        resolve(api.isFeatureSupported, "slIsFeatureSupported");
        resolve(api.setDevice, "slSetD3DDevice");
        resolve(api.upgradeInterface, "slUpgradeInterface");
        resolve(api.getNativeInterface, "slGetNativeInterface");
        resolve(api.getFeatureFunction, "slGetFeatureFunction");
        resolve(api.getFrameToken, "slGetNewFrameToken");
        resolve(api.setTags, "slSetTagForFrame");
        resolve(api.setConstants, "slSetConstants");

        // Manual hooking: Streamline sees only the device, queue and swap chain the presenter gives it.
        const auto plugins = directory.wstring();
        const wchar_t* paths[]{plugins.c_str()};
        const sl::Feature features[]{sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
        sl::Preferences preferences{};
        preferences.pathsToPlugins = paths;
        preferences.numPathsToPlugins = 1;
        preferences.pathToLogsAndData = paths[0];
        preferences.logMessageCallback = &logMessage;
        preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking
            | sl::PreferenceFlags::eUseDXGIFactoryProxy | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        preferences.featuresToLoad = features;
        preferences.numFeaturesToLoad = static_cast<std::uint32_t>(std::size(features));
        preferences.engine = sl::EngineType::eCustom;
        preferences.engineVersion = "4.11-Bend";
        preferences.projectId = ProjectId;
        check(api.init(preferences, sl::kSDKVersion), "Starting Streamline");
        initialized = true;

        DXGI_ADAPTER_DESC description{};
        if (FAILED(adapter->GetDesc(&description))) throw std::runtime_error("Reading the GPU's description failed.");
        sl::AdapterInfo info{};
        info.deviceLUID = reinterpret_cast<std::uint8_t*>(&description.AdapterLuid);
        info.deviceLUIDSizeInBytes = sizeof(LUID);
        if (const auto result = api.isFeatureSupported(sl::kFeatureDLSS_G, info); result != sl::Result::eOk)
            throw std::runtime_error(std::string("DLSS Frame Generation is not supported here (") + sl::getResultAsStr(result)
                + "): it needs an RTX 40 or newer GPU, a recent driver and hardware-accelerated GPU scheduling.");
        setStatus("Starting with the game's swap chain.");
        return true;
    }
    catch (const std::exception& error)
    {
        stop();
        setStatus(error.what());
        reshade::log::message(reshade::log::level::warning, (std::string("Days Gone DLSS: ") + error.what()).c_str());
        return false;
    }
}

bool attach(ID3D12Device* device) noexcept
{
    try
    {
        check(api.setDevice(device), "Giving Streamline the Direct3D 12 device");
        resolve(sl::kFeatureDLSS_G, api.setOptions, "slDLSSGSetOptions");
        resolve(sl::kFeatureDLSS_G, api.getState, "slDLSSGGetState");
        resolve(sl::kFeatureReflex, api.setReflexOptions, "slReflexSetOptions");
        resolve(sl::kFeatureReflex, api.getReflexState, "slReflexGetState");
        resolve(sl::kFeatureReflex, api.sleep, "slReflexSleep");
        resolve(sl::kFeaturePCL, api.setMarker, "slPCLSetMarker");
        // Frame generation paces its frames with Reflex.
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        check(api.setReflexOptions(reflex), "Turning on Reflex");
        attached = true;
        simulationMarked = false;
        applied = {};
        previous = {};
        return true;
    }
    catch (const std::exception& error)
    {
        setStatus(error.what());
        reshade::log::message(reshade::log::level::warning, (std::string("Days Gone DLSS: ") + error.what()).c_str());
        return false;
    }
}

IUnknown* upgrade(IUnknown* native) noexcept
{
    // The proxy holds its own reference to the native object; when Streamline makes none, the
    // object comes back as it is and gets one for the caller.
    void* object = native;
    if (api.upgradeInterface(&object) != sl::Result::eOk) return nullptr;
    if (object == native) native->AddRef();
    return static_cast<IUnknown*>(object);
}

IUnknown* nativeOf(IUnknown* proxy) noexcept
{
    void* object{};
    return api.getNativeInterface(proxy, &object) == sl::Result::eOk ? static_cast<IUnknown*>(object) : nullptr;
}

void stop() noexcept
{
    // The game and render threads stop marking first.
    attached = false;
    if (initialized) api.shutdown();
    initialized = false;
    applied = {};
}

void beginSimulation(std::uint32_t frame) noexcept
{
    if (!attached) return;

    simulationMarked = true;
    if (auto* frameToken = token(frame)) report(api.sleep(*frameToken), "Reflex sleep");
    marker(sl::PCLMarker::eSimulationStart, frame);
}

void endSimulation(std::uint32_t frame) noexcept
{
    if (attached) marker(sl::PCLMarker::eSimulationEnd, frame);
}

void beginRender(std::uint32_t frame) noexcept
{
    if (attached) marker(sl::PCLMarker::eRenderSubmitStart, frame);
}

void tag(ID3D12GraphicsCommandList* list, const Inputs& inputs, ID3D12Resource* depth, ID3D12Resource* motion,
    ID3D12Resource* hudless) noexcept
{
    if (!attached || !inputs.valid || !inputs.frame) return;

    auto* frameToken = token(inputs.frame);
    if (!frameToken) return;
    // The shared textures change with the next frame, so Streamline copies them now, in `list`.
    sl::Resource depthResource{sl::ResourceType::eTex2d, depth, D3D12_RESOURCE_STATE_COMMON};
    sl::Resource motionResource{sl::ResourceType::eTex2d, motion, D3D12_RESOURCE_STATE_COMMON};
    sl::Resource hudlessResource{sl::ResourceType::eTex2d, hudless, D3D12_RESOURCE_STATE_COMMON};
    const sl::Extent render{0, 0, inputs.renderWidth, inputs.renderHeight};
    const sl::ResourceTag tags[]{
        {&depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &render},
        {&motionResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &render},
        {&hudlessResource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow},
    };
    report(api.setTags(*frameToken, Viewport, tags, static_cast<std::uint32_t>(std::size(tags)), list),
        "Tagging the frame's inputs");
}

void beginPresent(const Inputs& inputs) noexcept
{
    if (!attached) return;

    applyMode(inputs.valid && inputs.frame != 0);
    updateStatus();
    auto* frameToken = inputs.frame ? token(inputs.frame) : nullptr;
    if (!frameToken) return;
    if (inputs.valid)
        report(api.setConstants(constants(inputs.frame, inputs), *frameToken, Viewport), "Setting the camera constants");
    marker(sl::PCLMarker::eRenderSubmitEnd, inputs.frame);
    marker(sl::PCLMarker::ePresentStart, inputs.frame);
}

void endPresent(const Inputs& inputs) noexcept
{
    if (!attached || !inputs.frame) return;

    marker(sl::PCLMarker::ePresentEnd, inputs.frame);
    // Without the game thread's hook the next frame's sleep and simulation are marked here, which
    // delays the frame after the game has read its input.
    if (simulationMarked) return;
    const auto next = inputs.frame + 1;
    if (auto* frameToken = token(next)) report(api.sleep(*frameToken), "Reflex sleep");
    marker(sl::PCLMarker::eSimulationStart, next);
    marker(sl::PCLMarker::eSimulationEnd, next);
}

void select(unsigned multiplier, bool dynamic) noexcept
{
    wantedMultiplier = multiplier;
    wantedDynamic = dynamic;
}

Capabilities capabilities() noexcept
{
    return {maxMultiplier, dynamicSupported};
}

bool running() noexcept
{
    return attached;
}

std::string status()
{
    std::lock_guard lock(statusMutex);
    return statusText;
}
}
