// DLSS settings UI, drawn inside the MP mod's F9 menu or as a ReShade overlay page.
#include "upscaler.hpp"

#include <imgui.h>
#if defined(DGMP_RESHADE_ADDON)
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#endif
#include <string>

namespace dgmp::upscaler
{
void drawMenu(float scale)
{
    auto wanted = currentSettings();
    auto options = currentDiagnostics();
    const auto state = status();
    bool changed{};
    ImGui::TextWrapped("NVIDIA DLSS replaces the game's temporal anti-aliasing. Below DLAA it renders at a lower resolution "
        "and upscales, overriding the game's Render Scale option while it is on.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(260 * scale);
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
    if (wanted.quality == Quality::Custom)
    {
        float percent = wanted.customScale * 100;
        ImGui::SetNextItemWidth(260 * scale);
        if (ImGui::SliderFloat("Render resolution", &percent, Settings::MinScale * 100, Settings::MaxScale * 100, "%.0f%%",
            ImGuiSliderFlags_AlwaysClamp))
        {
            wanted.customScale = percent / 100;
            changed = true;
        }
    }
    ImGui::SetNextItemWidth(260 * scale);
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
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Default lets DLSS choose: K for DLAA to Balanced, M for Performance and L for Ultra Performance.");
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::SliderFloat("Texture detail bias", &wanted.mipBiasOffset, Settings::MinMipBias, Settings::MaxMipBias, "%+.2f",
        ImGuiSliderFlags_AlwaysClamp))
        changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Added to the automatic mip bias for the render scale. Lower is sharper but can shimmer. Default: 0.");
    if (changed) applySettings(wanted);

    ImGui::Spacing();
    if (!state.active) ImGui::TextWrapped("DLSS is disabled.");
    else if (state.failed) ImGui::TextWrapped("DLSS stopped: %s Choose a mode again to retry.", state.failure.c_str());
    else if (wanted.quality == Quality::Off) ImGui::TextWrapped("Using the game's anti-aliasing.");
    else if (state.ngxFailed) ImGui::TextWrapped("%s", state.ngxMessage.c_str());
    else if (!state.shadersReady) ImGui::TextWrapped("Waiting for the game to render its anti-aliasing passes.");
    else if (state.planned)
    {
        ImGui::Text("Rendering %ux%u, output %ux%u (%.0f%%).", state.renderWidth, state.renderHeight, state.outputWidth,
            state.outputHeight, 100.0f * state.renderWidth / state.outputWidth);
        if (state.running) ImGui::TextUnformatted("DLSS is active.");
        else ImGui::TextWrapped("DLSS is waiting: %s", state.lastFallback.empty() ? "no frame yet." : state.lastFallback.c_str());
    }
    else ImGui::TextUnformatted("Starting DLSS...");

    if (!ImGui::CollapsingHeader("DLSS diagnostics")) return;
    bool diagnosticsChanged{};
    diagnosticsChanged |= ImGui::Checkbox("Invert jitter X", &options.invertJitterX);
    ImGui::SameLine();
    diagnosticsChanged |= ImGui::Checkbox("Invert jitter Y", &options.invertJitterY);
    diagnosticsChanged |= ImGui::Checkbox("Motion vectors include jitter", &options.jitteredMotion);
    const char* views[]{"Final image", "Motion vectors", "Depth", "DLSS input colour"};
    ImGui::SetNextItemWidth(220 * scale);
    diagnosticsChanged |= ImGui::Combo("Debug view", &options.debugView, views, static_cast<int>(std::size(views)));
    if (diagnosticsChanged) applyDiagnostics(options);
    ImGui::Text("Front end: %s; NGX context: %s", state.frontEnd.c_str(), state.wrappedContext ? "ReShade wrapper" : "native");
    ImGui::Text("Frames: %llu evaluated, %llu on game TAA", static_cast<unsigned long long>(state.evaluations),
        static_cast<unsigned long long>(state.fallbacks));
    if (!state.lastFallback.empty()) ImGui::TextWrapped("Last fallback: %s", state.lastFallback.c_str());
    ImGui::Text("Jitter %.3f, %.3f px; view %dx%d of %dx%d%s", state.jitter[0], state.jitter[1], state.viewRender[0],
        state.viewRender[1], state.viewOutput[0], state.viewOutput[1], state.cameraCut ? ", camera cut" : "");
    ImGui::Text("Mip bias %+.2f", state.mipBias);
    for (const auto& shader : state.shaders) ImGui::TextUnformatted(shader.c_str());
}
}
