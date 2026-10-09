// ReShade add-on front end for the DLSS core (DaysGoneDLSS.addon64). ReShade's events replace the
// MP mod's MinHook detours, and NGX receives ReShade's wrapped context so other add-ons that
// follow DLSS evaluations, such as RenoDX, see it like a game's own DLSS.
#include "upscaler.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <d3d11.h>
#include <MinHook.h>
#include <imgui.h>
#include <reshade.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <string>

extern "C" __declspec(dllexport) const char* NAME = "Days Gone DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "NVIDIA DLSS Super Resolution and DLAA in place of Days Gone's temporal anti-aliasing (Steam build 19221447).";

namespace
{
using namespace dgmp;
using namespace reshade::api;

// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

HMODULE self{};
bool engineReady{}, engineFailed{};
ID3D11Device* nativeDevice{};
ID3D11DeviceContext* nativeContext{};
ID3D11DeviceContext* wrappedContext{};

ID3D11DeviceContext* native(command_list* list)
{
    return reinterpret_cast<ID3D11DeviceContext*>(list->get_native());
}

bool supportedGame()
{
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == 0x68754D46
        && nt->OptionalHeader.SizeOfImage == 0x93F0000;
}

// Settings and logs follow the client profile: -saveddirsuffix=DGMP_<client>, otherwise "main".
std::filesystem::path dataDirectory()
{
    std::wstring client = L"main";
    const std::wstring line = GetCommandLineW();
    const std::wstring key = L"-saveddirsuffix=dgmp_";
    std::wstring lower(line);
    for (auto& character : lower) character = static_cast<wchar_t>(std::towlower(character));
    if (const auto found = lower.find(key); found != lower.npos)
    {
        auto end = found + key.size();
        while (end < line.size() && (std::iswalnum(line[end]) || line[end] == L'_')) ++end;
        if (end > found + key.size()) client = lower.substr(found + key.size(), end - found - key.size());
    }
    wchar_t* local{};
    std::size_t length{};
    std::filesystem::path root = _wdupenv_s(&local, &length, L"LOCALAPPDATA") == 0 && local ? local : L".";
    free(local);
    auto directory = root / L"DaysGoneMP" / client;
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
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
    // Keep the game's device: tools such as the MP mod's overlay create short-lived probe devices later.
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
    nativeContext = wrappedContext = nullptr;
    nativeDevice = nullptr;
}

void onInitPipeline(device* device, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipeline)
{
    if (device->get_api() != device_api::d3d11) return;
    for (uint32_t i = 0; i < count; ++i)
        if (subobjects[i].type == pipeline_subobject_type::compute_shader || subobjects[i].type == pipeline_subobject_type::pixel_shader)
        {
            const auto* shader = static_cast<const shader_desc*>(subobjects[i].data);
            if (shader) upscaler::shaderCreated(shader->code, shader->code_size, reinterpret_cast<IUnknown*>(pipeline.handle));
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
    return upscaler::dispatch(native(list), x, y, z);
}

bool onDraw(command_list* list, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t firstInstance)
{
    return instances == 1 && !firstInstance && upscaler::draw(native(list), vertices, first, 0, false);
}

bool onDrawIndexed(command_list* list, uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance)
{
    return instances == 1 && !firstInstance && upscaler::draw(native(list), indices, first, offset, true);
}

void onPresent(command_queue*, swapchain* chain, const rect*, const rect*, uint32_t, const rect*)
{
    // Other add-ons can present their own swap chains; count only the game's frames.
    if (chain->get_device()->get_api() != device_api::d3d11
        || (nativeDevice && reinterpret_cast<ID3D11Device*>(chain->get_device()->get_native()) != nativeDevice))
        return;
    if (!engineReady && !engineFailed)
    {
        // The engine is fully up by the first present; MinHook and the view hook are set up here.
        try
        {
            const auto status = MH_Initialize();
            if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) throw std::runtime_error(MH_StatusToString(status));
            upscaler::initialize(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), dataDirectory(), "ReShade add-on");
            engineReady = true;
        }
        catch (const std::exception& error)
        {
            engineFailed = true;
            reshade::log::message(reshade::log::level::error, (std::string("Days Gone DLSS: ") + error.what()).c_str());
        }
    }
    if (!engineReady) return;
    upscaler::tick();
    if (!upscaler::attached() && upscaler::wanted() && nativeContext)
        upscaler::attach(nativeDevice, nativeContext, wrappedContext);
    upscaler::endFrame();
}

void onOverlay(effect_runtime*)
{
    if (engineFailed) ImGui::TextWrapped("Days Gone DLSS could not start; see ReShade.log.");
    else if (!engineReady) ImGui::TextUnformatted("Days Gone DLSS starts with the first frame.");
    else upscaler::drawMenu(1.0f);
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshadeModule)
{
    if (!supportedGame() || !reshade::register_addon(addon, reshadeModule)) return false;
    self = addon;
    // The view hook and pending RHI commands point into this module; keep it loaded until exit.
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&AddonInit), &pinned);
    reshade::register_event<reshade::addon_event::init_command_queue>(onInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(onDestroyCommandQueue);
    reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
    reshade::register_event<reshade::addon_event::push_descriptors>(onPushDescriptors);
    reshade::register_event<reshade::addon_event::dispatch>(onDispatch);
    reshade::register_event<reshade::addon_event::draw>(onDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
    reshade::register_event<reshade::addon_event::present>(onPresent);
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
    nativeContext = wrappedContext = nullptr;
    nativeDevice = nullptr;
    reshade::unregister_overlay("Days Gone DLSS", onOverlay);
    reshade::unregister_addon(addon, reshadeModule);
}
