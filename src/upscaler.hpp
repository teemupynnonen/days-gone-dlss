#pragma once

#include "upscaler_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11SamplerState;
struct IUnknown;

// NVIDIA DLSS in place of Bend's temporal AA; README.md describes the render pipeline it hooks.
// The ReShade add-on (upscaler_addon.cpp) forwards the immediate context's calls to it.
namespace days_gone_dlss::upscaler
{
struct Diagnostics
{
    bool invertJitterX{}, invertJitterY{true}, jitteredMotion{};
    bool gameTemporalAA{}; // Comparison only: leave the frame to the game's TAA at the DLSS render scale.
    int debugView{};
};

struct Status
{
    bool active{}, failed{}, ngxFailed{}, shadersReady{}, planned{}, running{}, wrappedContext{};
    std::string failure, ngxMessage, lastFallback;
    unsigned renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    std::uint64_t evaluations{}, fallbacks{};
    float jitter[2]{};
    int viewRender[2]{}, viewOutput[2]{};
    bool cameraCut{};
    float mipBias{};
    std::array<std::string, 9> shaders;
};

// ---- Engine side ----
// Outside the loader lock, after MinHook is initialized: settings, console variables, view hook.
void initialize(std::uintptr_t imageBase, const std::filesystem::path& directory);
// Keeps the render scale and jitter in step with DLSS; called at present.
void tick() noexcept;
// Restores the engine's render settings and stops replacing the game's TAA.
void disable() noexcept;
// Re-enables after disable() without installing the engine hook again.
void resume() noexcept;
// Waits briefly for the render thread to release DLSS, then removes the engine hook.
void shutdown() noexcept;
// True while a DLSS mode is selected; the add-on attaches to Direct3D lazily.
bool wanted() noexcept;

// ---- Render side, on the thread executing Direct3D work ----
// `context` is the native immediate context every call below runs on. `ngxContext` is what NGX
// receives: ReShade's wrapper where there is one, so other add-ons can follow the evaluation.
void attach(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11DeviceContext* ngxContext) noexcept;
// Releases DLSS immediately; only when no Direct3D work can run (device teardown).
void detach() noexcept;
bool attached() noexcept;
// Each returns true when DLSS handled the call and the game's own call must be skipped.
bool dispatch(ID3D11DeviceContext* context, unsigned x, unsigned y, unsigned z) noexcept;
bool draw(ID3D11DeviceContext* context, unsigned count, unsigned start, int base, bool indexed) noexcept;
void pixelShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader) noexcept;
// Fills `biased` and returns true when any sampler should carry the DLSS mip bias.
bool biasSamplers(ID3D11DeviceContext* context, unsigned count, ID3D11SamplerState* const* samplers,
    ID3D11SamplerState** biased) noexcept;
// Recognises the game's shaders by their DXBC hash when they are created.
void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Once per presented frame, before overlays draw.
void endFrame() noexcept;

// ---- Settings UI (upscaler_menu.cpp), on the render thread ----
Status status();
Settings currentSettings();
void applySettings(const Settings& settings);
Diagnostics currentDiagnostics();
void applyDiagnostics(const Diagnostics& diagnostics);
void drawMenu(float scale);
}
