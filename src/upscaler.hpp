#pragma once

#include "frame_generation.hpp"
#include "upscaler_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11SamplerState;
struct IUnknown;

// NVIDIA DLSS or AMD FSR (fsr_upscaler.hpp) in place of Bend's temporal AA; docs/DEVELOPMENT.md
// describes the render pipeline it hooks. The ReShade add-on (upscaler_addon.cpp) forwards the
// immediate context's calls to it.
namespace days_gone_dlss::upscaler
{
struct Diagnostics
{
    bool invertJitterX{}, invertJitterY{true}, jitteredMotion{};
    bool gameTemporalAA{}; // Comparison only: leave the frame to the game's TAA at the DLSS render scale.
    bool fullResolutionAmbientOcclusion{true};
    int debugView{};
};

struct Status
{
    bool active{}, failed{}, shadersReady{}, planned{}, running{}, wrappedContext{};
    bool indirectLightingOption{}; // The game build has the indirect lighting setting (1.07)
    // The chosen upscaler's name ("DLSS", "FSR 3.1.5") and whether it could not start, and why.
    std::string upscalerName;
    bool upscalerFailed{};
    std::string upscalerMessage;
    std::string gameBuild, failure, lastFallback;
    unsigned renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    std::uint64_t evaluations{}, fallbacks{};
    float jitter[2]{};
    int viewRender[2]{}, viewOutput[2]{};
    bool cameraCut{};
    float mipBias{};
    unsigned ambientShaders{}, ambientShaderFailures{};
    std::array<std::string, 9> shaders;
    unsigned shadersRecognised{};
    bool shaderMismatch{}; // A game shader has unexpected bytecode, so DLSS stays off.
};

// ---- Engine side ----
// Recognises the game build from the executable's PE header, before the game creates its shaders.
// Returns the build's name, or nullptr when the executable is not a supported build.
const char* selectBuild(std::uintptr_t imageBase) noexcept;
// The ray-tracing experiment only knows the selected build's view layout on 1.13.
bool rayTracingSupported() noexcept;
// Outside the loader lock, after MinHook is initialized and a build is selected: settings, console
// variables, view hook. With Reflex (DLSS Frame Generation) also the game thread's frame.
void initialize(const std::filesystem::path& directory, bool withReflex);
// Keeps the render scale and jitter in step with the upscaler; called at present.
void tick() noexcept;
// Restores the engine's render settings and stops replacing the game's TAA.
void disable() noexcept;
// Re-enables after disable() without installing the engine hook again.
void resume() noexcept;
// Waits briefly for the render thread to release the upscaler, then removes the engine hook.
void shutdown() noexcept;
// True while an upscaling mode is selected; the add-on attaches to Direct3D lazily.
bool wanted() noexcept;
// Appends a line to DaysGoneDLSS.log beside the add-on, from any thread.
void log(std::string_view message);

// ---- Render side, on the thread executing Direct3D work ----
// `context` is the native immediate context every call below runs on. `ngxContext` is what NGX
// receives: ReShade's wrapper where there is one, so other add-ons can follow the evaluation.
// `frameGeneration` shares the upscaler's depth and motion with Direct3D 12 and adds the hudless colour.
void attach(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11DeviceContext* ngxContext, bool frameGeneration) noexcept;
// Releases the upscaler immediately; only when no Direct3D work can run (device teardown).
void detach() noexcept;
bool attached() noexcept;
// Each returns true when the upscaler handled the call and the game's own call must be skipped.
bool dispatch(ID3D11DeviceContext* context, unsigned x, unsigned y, unsigned z) noexcept;
bool draw(ID3D11DeviceContext* context, unsigned count, unsigned start, int base, bool indexed) noexcept;
void pixelShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader) noexcept;
// Fills `biased` and returns true when any sampler should carry the upscaler's mip bias.
bool biasSamplers(ID3D11DeviceContext* context, unsigned count, ID3D11SamplerState* const* samplers,
    ID3D11SamplerState** biased) noexcept;
// Recognises the game's shaders by their DXBC hash when they are created.
void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// The presented frame's inputs for frame generation; not valid when upscaling did not run on it. Before endFrame().
frame_generation::Inputs frameGenerationInputs() noexcept;
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
