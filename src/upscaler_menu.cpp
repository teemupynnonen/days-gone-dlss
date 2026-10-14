// DLSS settings UI, drawn as a ReShade overlay window.
#include "upscaler.hpp"

#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <format>
#include <string>

namespace days_gone_dlss::upscaler
{
namespace
{
const ImVec4 Grey{0.6f, 0.6f, 0.6f, 1}, Green{0.45f, 0.85f, 0.45f, 1}, Yellow{0.95f, 0.8f, 0.35f, 1}, Red{0.95f, 0.4f, 0.4f, 1};

void tooltip(const char* text)
{
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void statusLine(const ImVec4& colour, const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// One coloured line on what DLSS is doing right now.
void drawStatus(const Status& state, Quality quality)
{
    if (!state.active) statusLine(Grey, "DLSS is disabled.");
    else if (state.failed) statusLine(Red, std::format("DLSS stopped: {} Choose a mode again to retry.", state.failure));
    else if (quality == Quality::Off) statusLine(Grey, "DLSS is off. The game's own anti-aliasing is in use.");
    else if (state.ngxFailed) statusLine(Red, state.ngxMessage);
    else if (!state.shadersReady && state.shaderMismatch)
        statusLine(Red, "This game version's anti-aliasing shaders are not the ones DLSS expects, so DLSS stays off.");
    else if (!state.shadersReady) statusLine(Yellow, "Waiting for the game to draw its anti-aliasing.");
    else if (!state.planned) statusLine(Yellow, "Starting DLSS...");
    else if (!state.running)
        statusLine(Yellow, std::format("DLSS is waiting: {}.", state.lastFallback.empty() ? "no frame yet" : state.lastFallback));
    else if (state.renderWidth == state.outputWidth)
        statusLine(Green, std::format("DLSS active at {}x{}, native resolution.", state.outputWidth, state.outputHeight));
    else
        statusLine(Green, std::format("DLSS active: {}x{} upscaled to {}x{} ({:.0f}%).", state.renderWidth, state.renderHeight,
            state.outputWidth, state.outputHeight, 100.0f * state.renderWidth / state.outputWidth));
}

// Returns true when a setting changed.
bool drawSettings(Settings& wanted, const Status& state, float width)
{
    bool changed{};
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("DLSS mode", std::string(label(wanted.quality)).c_str()))
    {
        for (int i = 0; i < static_cast<int>(Quality::Count); ++i)
        {
            const auto quality = static_cast<Quality>(i);
            if (ImGui::Selectable(std::string(label(quality)).c_str(), quality == wanted.quality))
            {
                wanted.quality = quality;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    tooltip("DLAA renders at full resolution. Lower modes render fewer pixels and upscale them, for higher frame rates.\n"
        "DLSS replaces the game's anti-aliasing and overrides its Render Scale option while it is on.");

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

    // The rest only applies while DLSS runs.
    ImGui::BeginDisabled(wanted.quality == Quality::Off);
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

    float sharpening = wanted.sharpness * 100;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Sharpening", &sharpening, 0, Settings::MaxSharpness * 100, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
    {
        wanted.sharpness = sharpening / 100;
        changed = true;
    }
    tooltip("AMD FidelityFX RCAS on the DLSS output. The game's own anti-aliasing sharpens; DLSS does not. 0% is off.");

    ImGui::SetNextItemWidth(width);
    changed |= ImGui::SliderFloat("Texture detail bias", &wanted.mipBiasOffset, Settings::MinMipBias, Settings::MaxMipBias, "%+.2f",
        ImGuiSliderFlags_AlwaysClamp);
    tooltip("Added to the automatic mip bias for the render scale. Lower is sharper but can shimmer.");

    if (state.indirectLightingOption)
    {
        changed |= ImGui::Checkbox("Screen-space indirect lighting", &wanted.indirectLighting);
        tooltip("Bounced light between nearby surfaces, from the game's highest settings. It is noisy and can make\n"
            "foliage shimmer at lower DLSS render resolutions. Off renders as the game's lower settings do.");
    }

    Settings defaults;
    defaults.quality = wanted.quality;
    defaults.customScale = wanted.customScale;
    ImGui::BeginDisabled(wanted == defaults);
    if (ImGui::Button("Reset to defaults"))
    {
        wanted = defaults;
        changed = true;
    }
    ImGui::EndDisabled();
    tooltip("Resets everything except the DLSS mode.");
    ImGui::EndDisabled();

    return changed;
}

void drawDiagnostics(const Status& state, float width)
{
    ImGui::TextDisabled("For troubleshooting. Leave these at their defaults.");
    auto options = currentDiagnostics();
    bool changed{};
    const char* views[]{"Final image", "Motion vectors", "Depth", "DLSS input colour", "Current colour bias"};
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
    ImGui::Text("Frames: %llu with DLSS, %llu with the game's TAA", static_cast<unsigned long long>(state.evaluations),
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

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Diagnostics")) drawDiagnostics(state, width);
}
}
