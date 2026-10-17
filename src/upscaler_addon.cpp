// ReShade add-on for the DLSS core (DaysGoneDLSS.addon64). ReShade's events feed the core the
// game's Direct3D calls, and NGX receives ReShade's wrapped context so other add-ons that
// follow DLSS evaluations, such as RenoDX, see it like a game's own DLSS.
#include "upscaler.hpp"
#include "fsr_frame_generation.hpp"
#include "presenter.hpp"
#include "ray_tracing.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <d3d11.h>
#include <MinHook.h>
#include <imgui.h>
#include <reshade.hpp>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

extern "C" __declspec(dllexport) const char* NAME = "Days Gone DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "NVIDIA DLSS or AMD FSR upscaling and frame generation in place of Days Gone's temporal anti-aliasing (game versions 1.13 and 1.07).";

namespace
{
using namespace days_gone_dlss;
using namespace reshade::api;

// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

HMODULE self{};
bool engineReady{}, engineFailed{};
void* exceptionHandler{}; // TEMPORARY
ID3D11Device* nativeDevice{};
ID3D11DeviceContext* nativeContext{};
ID3D11DeviceContext* wrappedContext{};

ID3D11DeviceContext* native(command_list* list)
{
    return reinterpret_cast<ID3D11DeviceContext*>(list->get_native());
}

// Settings and the log live beside the add-on, as ReShade keeps its own.
std::filesystem::path dataDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < path.size()) return std::filesystem::path(path.substr(0, length)).parent_path();
        path.resize(path.size() * 2);
    }
}

// The command queue ReShade reports is a base inside its D3D11DeviceContext proxy; find the
// proxy itself and prove it by asking for the native context it wraps.
ID3D11DeviceContext* wrapperOf(command_queue* queue, ID3D11DeviceContext* context)
{
    const auto address = reinterpret_cast<std::uintptr_t>(queue);
    HMODULE reshade{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        *reinterpret_cast<LPCWSTR*>(address), &reshade))
        return nullptr;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), reshade, &info, sizeof(info))) return nullptr;
    const auto begin = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), end = begin + info.SizeOfImage;
    for (std::uintptr_t offset = 8; offset <= 32; offset += 8)
    {
        const auto candidate = address - offset;
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(candidate);
        if (vtable < begin || vtable >= end) continue;
        IUnknown* unwrapped{};
        if (FAILED(reinterpret_cast<IUnknown*>(candidate)->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(&unwrapped))))
            continue;
        const bool match = unwrapped == context;
        unwrapped->Release();
        if (match) return reinterpret_cast<ID3D11DeviceContext*>(candidate);
    }
    return nullptr;
}

void onInitCommandQueue(command_queue* queue)
{
    if (queue->get_device()->get_api() != device_api::d3d11) return;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(queue->get_native());
    // Keep the game's device: overlays and other tools can create short-lived probe devices later.
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || nativeContext) return;
    nativeContext = context;
    nativeDevice = reinterpret_cast<ID3D11Device*>(queue->get_device()->get_native());
    wrappedContext = wrapperOf(queue, context);
    char text[160]{};
    std::snprintf(text, sizeof(text), "Days Gone DLSS: immediate context %p, ReShade wrapper %p.", static_cast<void*>(context),
        static_cast<void*>(wrappedContext));
    reshade::log::message(wrappedContext ? reshade::log::level::info : reshade::log::level::warning, text);
}

void onDestroyCommandQueue(command_queue* queue)
{
    if (reinterpret_cast<ID3D11DeviceContext*>(queue->get_native()) != nativeContext) return;
    upscaler::detach();
    if (upscaler::rayTracingArmed()) ray_tracing::release();
    nativeContext = wrappedContext = nullptr;
    nativeDevice = nullptr;
}

void onInitPipeline(device* device, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipeline)
{
    if (device->get_api() != device_api::d3d11) return;
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* shader = static_cast<const shader_desc*>(subobjects[i].data);
        if (!shader) continue;
        auto* object = reinterpret_cast<IUnknown*>(pipeline.handle);
        if (subobjects[i].type == pipeline_subobject_type::compute_shader || subobjects[i].type == pipeline_subobject_type::pixel_shader)
            upscaler::shaderCreated(shader->code, shader->code_size, object);
        if (!upscaler::rayTracingArmed()) continue;
        if (subobjects[i].type == pipeline_subobject_type::pixel_shader)
            ray_tracing::pixelShaderCreated(shader->code, shader->code_size, object);
        else if (subobjects[i].type == pipeline_subobject_type::vertex_shader)
            ray_tracing::vertexShaderCreated(shader->code, shader->code_size, object);
        else if (subobjects[i].type == pipeline_subobject_type::compute_shader)
            ray_tracing::computeShaderCreated(shader->code, shader->code_size, object);
    }
}

void onBindPipeline(command_list* list, pipeline_stage stages, pipeline pipeline)
{
    if ((stages & pipeline_stage::pixel_shader) == pipeline_stage::pixel_shader)
        upscaler::pixelShader(native(list), reinterpret_cast<ID3D11PixelShader*>(pipeline.handle));
}

// ReShade reports bound samplers after binding them; rebind biased copies on the native context.
void onPushDescriptors(command_list* list, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& update)
{
    if (update.type != descriptor_type::sampler || (stages & shader_stage::pixel) != shader_stage::pixel
        || update.count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return;
    std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> biased{};
    auto* context = native(list);
    const auto* samplers = static_cast<ID3D11SamplerState* const*>(update.descriptors);
    if (upscaler::biasSamplers(context, update.count, samplers, biased.data()))
        context->PSSetSamplers(update.binding, update.count, biased.data());
}

bool onDispatch(command_list* list, uint32_t x, uint32_t y, uint32_t z)
{
    if (upscaler::rayTracingArmed()) ray_tracing::dispatch(native(list));
    return upscaler::dispatch(native(list), x, y, z);
}

bool onDraw(command_list* list, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t firstInstance)
{
    if (upscaler::rayTracingArmed() && ray_tracing::draw(native(list), vertices, instances, first, 0, firstInstance, false)) return true;
    return instances == 1 && !firstInstance && upscaler::draw(native(list), vertices, first, 0, false);
}

bool onDrawIndexed(command_list* list, uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance)
{
    if (upscaler::rayTracingArmed() && ray_tracing::draw(native(list), indices, instances, first, offset, firstInstance, true)) return true;
    return instances == 1 && !firstInstance && upscaler::draw(native(list), indices, first, offset, true);
}

// Once per game frame, as the game presents into `backBuffer`. Returns what frame generation needs
// from the frame.
frame_generation::Inputs frame(ID3D11Texture2D* backBuffer)
{
    if (!engineReady && !engineFailed)
    {
        // The engine is fully up by the first present; MinHook and the view hook are set up here.
        try
        {
            const auto status = MH_Initialize();
            if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) throw std::runtime_error(MH_StatusToString(status));
            upscaler::initialize(dataDirectory(), presenter::active() && presenter::route() == presenter::Route::Streamline);
            engineReady = true;
        }
        catch (const std::exception& error)
        {
            engineFailed = true;
            reshade::log::message(reshade::log::level::error, (std::string("Days Gone DLSS: ") + error.what()).c_str());
        }
    }
    if (!engineReady) return {};
    upscaler::tick();
    if (!upscaler::attached() && upscaler::wanted() && nativeContext)
        upscaler::attach(nativeDevice, nativeContext, wrappedContext, presenter::active());
    // FrameGeneration's fixed DLSS modes run from 2x at X2 = 1.
    const auto mode = upscaler::currentSettings().frameGeneration;
    const bool dynamic = mode == upscaler::FrameGeneration::Dynamic;
    const bool dlss = mode != upscaler::FrameGeneration::Off && mode != upscaler::FrameGeneration::Fsr;
    frame_generation::select(!dlss || dynamic ? 0 : static_cast<unsigned>(mode) + 1, dynamic);
    fsr_frame_generation::select(mode == upscaler::FrameGeneration::Fsr);
    const auto inputs = upscaler::frameGenerationInputs();
    upscaler::endFrame();
    if (upscaler::rayTracingArmed() && nativeContext) ray_tracing::present(nativeContext, backBuffer);

    return inputs;
}

void onPresent(command_queue*, swapchain* chain, const rect*, const rect*, uint32_t, const rect*)
{
    // Other add-ons can present their own swap chains; count only the game's frames. Through
    // Direct3D 12 the game's swap chain is the presenter's stand-in, which hands the frame on.
    if (chain->get_device()->get_api() != device_api::d3d11
        || (nativeDevice && reinterpret_cast<ID3D11Device*>(chain->get_device()->get_native()) != nativeDevice))
        return;
    const auto inputs = frame(reinterpret_cast<ID3D11Texture2D*>(chain->get_current_back_buffer().handle));
    if (presenter::active()) presenter::submit(inputs);
}

// F9 switches the ray-traced features off and on, for A/B comparison with the overlay closed.
void onReshadePresent(effect_runtime* runtime)
{
    if (engineReady && upscaler::rayTracingArmed() && runtime->is_key_pressed(VK_F9)) ray_tracing::toggleFeatures();
}

void onReshadeOverlay(effect_runtime*)
{
    if (engineReady && upscaler::rayTracingArmed()) ray_tracing::drawIndicator();
}

void onOverlay(effect_runtime*)
{
    if (engineFailed) ImGui::TextWrapped("Days Gone DLSS could not start; see ReShade.log.");
    else if (!engineReady) ImGui::TextUnformatted("Days Gone DLSS starts with the first frame.");
    else upscaler::drawMenu(1.0f);
}

// The game's swap chain is created once, early, and ray tracing prepares each shader as the game
// creates it, so frame generation and ray tracing are decided from the settings at load.
upscaler::Settings startupSettings()
{
    try { return upscaler::loadSettings(dataDirectory() / "DaysGoneDLSS.ini"); }
    catch (const std::exception& error)
    {
        reshade::log::message(reshade::log::level::warning, (std::string("Days Gone DLSS: ") + error.what()).c_str());
        return {};
    }
}
}

// TEMPORARY: where fatal exceptions happen, before the engine's crash handler ends the process.
std::string where(DWORD64 address)
{
    HMODULE module{};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) || !GetModuleFileNameW(module, path, MAX_PATH))
        return std::to_string(address);
    char text[160]{};
    std::snprintf(text, sizeof(text), "%ls+0x%llx", std::filesystem::path(path).filename().c_str(),
        static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    return text;
}

LONG CALLBACK logFatalException(EXCEPTION_POINTERS* info)
{
    static int logged{};
    const auto code = info->ExceptionRecord->ExceptionCode;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION
            && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000374 && code != 0xC0000409) || ++logged > 4)
        return EXCEPTION_CONTINUE_SEARCH;

    char head[160]{};
    std::snprintf(head, sizeof(head), "DEBUG exception 0x%08lx, data %llx %llx; stack:", code,
        static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters > 0 ? info->ExceptionRecord->ExceptionInformation[0] : 0),
        static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0));
    std::string text = head;
    CONTEXT context = *info->ContextRecord;
    for (int frame = 0; frame < 20 && context.Rip; ++frame)
    {
        text += " " + where(context.Rip);
        DWORD64 imageBase{};
        auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function)
        {
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handlerData{};
        DWORD64 establisher{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
    }
    upscaler::log(text);
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshadeModule)
{
    if (!upscaler::selectBuild(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)))
        || !reshade::register_addon(addon, reshadeModule))
        return false;

    if (!exceptionHandler) exceptionHandler = AddVectoredExceptionHandler(1, logFatalException); // TEMPORARY
    self = addon;
    // The view hook and pending RHI commands point into this module; keep it loaded until exit.
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&AddonInit), &pinned);
    const auto settings = startupSettings();
    upscaler::armRayTracing(settings.rayTracing);
    if (const auto mode = settings.frameGeneration; mode != upscaler::FrameGeneration::Off)
        presenter::install(dataDirectory(), mode == upscaler::FrameGeneration::Fsr ? presenter::Route::Fsr : presenter::Route::Streamline);
    reshade::register_event<reshade::addon_event::init_command_queue>(onInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(onDestroyCommandQueue);
    reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
    reshade::register_event<reshade::addon_event::push_descriptors>(onPushDescriptors);
    reshade::register_event<reshade::addon_event::dispatch>(onDispatch);
    reshade::register_event<reshade::addon_event::draw>(onDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
    reshade::register_event<reshade::addon_event::present>(onPresent);
    reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresent);
    reshade::register_event<reshade::addon_event::reshade_overlay>(onReshadeOverlay);
    reshade::register_overlay("Days Gone DLSS", onOverlay);
    if (engineReady) upscaler::resume();
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE reshadeModule)
{
    // The game keeps rendering without the add-on; give it back its own TAA settings.
    upscaler::disable();
    // ReShade unloads add-ons when its last device goes; the next load reports the game's device again.
    upscaler::detach();
    if (upscaler::rayTracingArmed()) ray_tracing::release();
    nativeContext = wrappedContext = nullptr;
    nativeDevice = nullptr;
    reshade::unregister_overlay("Days Gone DLSS", onOverlay);
    reshade::unregister_addon(addon, reshadeModule);
    RemoveVectoredExceptionHandler(exceptionHandler); // TEMPORARY
    exceptionHandler = nullptr;
}
