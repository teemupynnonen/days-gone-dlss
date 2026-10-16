// Upscaler settings UI, drawn as a ReShade overlay window.
#include "upscaler.hpp"
#include "fsr_frame_generation.hpp"
#include "menu_style.hpp"
#include "presenter.hpp"
#include "ray_tracing.hpp"

#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <format>
#include <string>

namespace days_gone_dlss::upscaler
{
namespace
{
using namespace menu;

// One coloured line on what the upscaler is doing right now.
void drawStatus(const Status& state, Quality quality)
{
    const auto& name = state.upscalerName;
    if (!state.active) statusLine(Grey, "Upscaling is disabled.");
    else if (state.failed) statusLine(Red, std::format("{} stopped: {} Choose a mode again to retry.", name, state.failure));
    else if (quality == Quality::Off) statusLine(Grey, "Upscaling is off. The game's own anti-aliasing is in use.");
    else if (state.upscalerFailed) statusLine(Red, state.upscalerMessage);
    else if (!state.shadersReady && state.shaderMismatch)
        statusLine(Red, "This game version's anti-aliasing shaders are not the ones the add-on expects, so upscaling stays off.");
    else if (!state.shadersReady) statusLine(Yellow, "Waiting for the game to draw its anti-aliasing.");
    else if (!state.planned) statusLine(Yellow, std::format("Starting {}...", name));
    else if (!state.running)
        statusLine(Yellow, std::format("{} is waiting: {}.", name, state.lastFallback.empty() ? "no frame yet" : state.lastFallback));
    else if (state.renderWidth == state.outputWidth)
        statusLine(Green, std::format("{} active at {}x{}, native resolution.", name, state.outputWidth, state.outputHeight));
    else
        statusLine(Green, std::format("{} active: {}x{} upscaled to {}x{} ({:.0f}%).", name, state.renderWidth, state.renderHeight,
            state.outputWidth, state.outputHeight, 100.0f * state.renderWidth / state.outputWidth));
}

// Returns true when a setting changed.
bool drawSettings(Settings& wanted, const Status& state, float width)
{
    bool changed{};
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("Upscaler", std::string(label(wanted.upscaler)).c_str()))
    {
        for (int i = 0; i < static_cast<int>(Upscaler::Count); ++i)
        {
            const auto upscaler = static_cast<Upscaler>(i);
            if (ImGui::Selectable(std::string(label(upscaler)).c_str(), upscaler == wanted.upscaler))
            {
                wanted.upscaler = upscaler;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    tooltip("NVIDIA DLSS needs an NVIDIA RTX GPU.\n"
        "AMD FSR runs on any GPU: FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 on the rest.");

    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("Mode", std::string(label(wanted.quality, wanted.upscaler)).c_str()))
    {
        for (int i = 0; i < static_cast<int>(Quality::Count); ++i)
        {
            const auto quality = static_cast<Quality>(i);
            if (ImGui::Selectable(std::string(label(quality, wanted.upscaler)).c_str(), quality == wanted.quality))
            {
                wanted.quality = quality;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    tooltip("DLAA and Native AA render at full resolution. Lower modes render fewer pixels and upscale them, for higher\n"
        "frame rates. The upscaler replaces the game's anti-aliasing and overrides its Render Scale option while it is on.");

    if (wanted.quality == Quality::Custom)
    {
        float percent = wanted.customScale * 100;
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderFloat("Render resolution", &percent, Settings::MinScale * 100, Settings::MaxScale * 100, "%.0f%%",
            ImGuiSliderFlags_AlwaysClamp))
        {
            wanted.customScale = percent / 100;
            changed = true;
        }
    }

    // The rest only applies while the upscaler runs.
    ImGui::BeginDisabled(wanted.quality == Quality::Off);
    if (wanted.upscaler == Upscaler::Dlss)
    {
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("DLSS preset", std::string(label(wanted.preset)).c_str()))
        {
            for (const auto preset : {Preset::Default, Preset::K, Preset::J, Preset::M, Preset::L})
                if (ImGui::Selectable(std::string(label(preset)).c_str(), preset == wanted.preset))
                {
                    wanted.preset = preset;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        tooltip("Default lets DLSS choose: K for DLAA to Balanced, M for Performance and L for Ultra Performance.");
    }

    float sharpening = wanted.sharpness * 100;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Sharpening", &sharpening, 0, Settings::MaxSharpness * 100, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
    {
        wanted.sharpness = sharpening / 100;
        changed = true;
    }
    tooltip("AMD FidelityFX RCAS on the upscaled image. The game's own anti-aliasing sharpens; the upscalers do not. 0% is off.");

    ImGui::SetNextItemWidth(width);
    changed |= ImGui::SliderFloat("Texture detail bias", &wanted.mipBiasOffset, Settings::MinMipBias, Settings::MaxMipBias, "%+.2f",
        ImGuiSliderFlags_AlwaysClamp);
    tooltip("Added to the automatic mip bias for the render scale. Lower is sharper but can shimmer.");

    if (state.indirectLightingOption)
    {
        changed |= ImGui::Checkbox("Screen-space indirect lighting", &wanted.indirectLighting);
        tooltip("Bounced light between nearby surfaces, from the game's highest settings. It is noisy and can make\n"
            "foliage shimmer at lower render resolutions. Off renders as the game's lower settings do.");
    }

    // Until Streamline reports what the GPU supports, every DLSS mode is offered.
    const auto supported = frame_generation::capabilities();
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("Frame generation", std::string(label(wanted.frameGeneration)).c_str()))
    {
        for (int i = 0; i < static_cast<int>(FrameGeneration::Count); ++i)
        {
            const auto mode = static_cast<FrameGeneration>(i);
            bool unsupported{};
            if (mode == FrameGeneration::Dynamic) unsupported = supported.maxMultiplier && !supported.dynamic;
            else if (mode >= FrameGeneration::X2 && mode <= FrameGeneration::X6)
                unsupported = supported.maxMultiplier && static_cast<unsigned>(i) + 1 > supported.maxMultiplier;
            if (ImGui::Selectable(std::string(label(mode)).c_str(), mode == wanted.frameGeneration,
                unsupported ? ImGuiSelectableFlags_Disabled : 0))
            {
                wanted.frameGeneration = mode;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    tooltip("Frames generated between those the game renders, with either upscaler.\n"
        "DLSS Frame Generation: 2x needs an RTX 40, 3x to 6x and Dynamic an RTX 50. Dynamic chooses the\n"
        "multiplier that reaches the display's refresh rate. It turns on NVIDIA Reflex.\n"
        "FSR frame generation: 2x on any GPU, machine-learned on Radeon RX 9000.\n"
        "The game then presents through Direct3D 12, which is set up at startup, so turning it on or switching\n"
        "between DLSS and FSR takes a restart; other changes apply at once.");
    const bool fsrWanted = wanted.frameGeneration == FrameGeneration::Fsr;
    const bool fsrRoute = presenter::route() == presenter::Route::Fsr;
    if (presenter::active() && wanted.frameGeneration != FrameGeneration::Off && fsrWanted != fsrRoute)
        statusLine(Yellow, "Restart the game to switch frame generation.");
    else if (presenter::active()) statusLine(Grey, fsrRoute ? fsr_frame_generation::status() : frame_generation::status());
    else if (wanted.frameGeneration != FrameGeneration::Off)
    {
        const auto reason = presenter::status();
        statusLine(Yellow, reason.empty() ? "Restart the game to start frame generation." : reason);
    }

    // Frame generation starts with the game, so a reset leaves it alone like the upscaler and mode.
    Settings defaults;
    defaults.upscaler = wanted.upscaler;
    defaults.quality = wanted.quality;
    defaults.customScale = wanted.customScale;
    defaults.frameGeneration = wanted.frameGeneration;
    ImGui::BeginDisabled(wanted == defaults);
    if (ImGui::Button("Reset to defaults"))
    {
        wanted = defaults;
        changed = true;
    }
    ImGui::EndDisabled();
    tooltip("Resets everything except the upscaler, its mode and frame generation.");
    ImGui::EndDisabled();

    return changed;
}

void drawDiagnostics(const Status& state, float width)
{
    ImGui::TextDisabled("For troubleshooting. Leave these at their defaults.");
    auto options = currentDiagnostics();
    bool changed{};
    const char* views[]{"Final image", "Motion vectors", "Depth", "Upscaler input colour", "Current colour bias"};
    ImGui::SetNextItemWidth(width);
    changed |= ImGui::Combo("Debug view", &options.debugView, views, static_cast<int>(std::size(views)));
    changed |= ImGui::Checkbox("Invert jitter X", &options.invertJitterX);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Invert jitter Y", &options.invertJitterY);
    changed |= ImGui::Checkbox("Motion vectors include jitter", &options.jitteredMotion);
    changed |= ImGui::Checkbox("Correct ambient occlusion weights", &options.fullResolutionAmbientOcclusion);
    if (changed) applyDiagnostics(options);

    ImGui::Spacing();
    ImGui::Text("Game version %s", state.gameBuild.c_str());
    ImGui::Text("Frames: %llu upscaled, %llu with the game's TAA", static_cast<unsigned long long>(state.evaluations),
        static_cast<unsigned long long>(state.fallbacks));
    if (!state.lastFallback.empty()) ImGui::TextWrapped("Last fallback: %s", state.lastFallback.c_str());
    ImGui::Text("Jitter %.3f, %.3f px; view %dx%d of %dx%d%s", state.jitter[0], state.jitter[1], state.viewRender[0],
        state.viewRender[1], state.viewOutput[0], state.viewOutput[1], state.cameraCut ? ", camera cut" : "");
    ImGui::Text("Mip bias %+.2f; NGX context: %s", state.mipBias, state.wrappedContext ? "ReShade wrapper" : "native");
    ImGui::Text("Ambient lighting: %u shaders, %u failures", state.ambientShaders, state.ambientShaderFailures);

    const auto shaders = std::format("Game shaders: {} of {} recognised###shaders", state.shadersRecognised, state.shaders.size());
    if (ImGui::TreeNode(shaders.c_str()))
    {
        for (const auto& shader : state.shaders) ImGui::TextUnformatted(shader.c_str());
        ImGui::TreePop();
    }
}
}

void drawMenu(float scale)
{
    const float width = 260 * scale;
    auto wanted = currentSettings();
    const auto state = status();
    drawStatus(state, wanted.quality);
    ImGui::Separator();
    if (drawSettings(wanted, state, width)) applySettings(wanted);

    if (rayTracingSupported())
    {
        ImGui::Spacing();
        ray_tracing::drawMenu();
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Diagnostics"))
    {
        drawDiagnostics(state, width);
        if (rayTracingSupported())
        {
            ImGui::Spacing();
            ray_tracing::drawDiagnostics(width);
        }
    }
}
}
