// Ray-tracing experiment section of the ReShade overlay window, and its A/B indicator.
#include "ray_tracing.hpp"

#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <iterator>

namespace days_gone_dlss::ray_tracing
{
void drawMenu(float scale)
{
    if (!ImGui::CollapsingHeader("Ray tracing (experimental)")) return;

    const auto state = status();
    ImGui::TextWrapped("Traces the geometry the game draws to its G-buffer and sun shadow map. A step toward path tracing; it "
        "costs performance and is not meant for play.");
    auto features = state.features;
    if (ImGui::Checkbox("Ray-traced sun shadows", &features.sunShadows)) enable(features);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds sun shadows traced from the geometry on screen and the game's shadow casters around it to the game's "
            "own shadows.");
    ImGui::Indent();
    ImGui::BeginDisabled(!features.sunShadows);
    if (ImGui::Checkbox("Replace the game's soft tree shadows", &features.replaceCanopyShadows)) enable(features);
    ImGui::EndDisabled();
    ImGui::Unindent();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Leaves out the game's soft shadow under tree canopies, so only the traced trees and leaves shadow there.");
    ImGui::Indent();
    if (ImGui::Checkbox("Off-screen objects and trees", &features.offscreenGeometry)) enable(features);
    ImGui::Unindent();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Also traces cached static objects and copies of the trees around the camera, so their shadows stay when they "
            "leave the view. Off: only what is on screen and the game's shadow casters cast.");
    if (ImGui::Checkbox("Ray-traced ambient occlusion", &features.ambientOcclusion)) enable(features);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Replaces the game's screen-space ambient occlusion with occlusion traced from the same geometry, filtered by "
            "the game's own blur and accumulation.");
    if (ImGui::Checkbox("Ray-traced global illumination", &features.globalIllumination)) enable(features);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds sunlight bounced once off the same geometry to the game's lighting, accumulated over frames and "
            "filtered.");
    ImGui::TextWrapped("F9 switches the ray-traced features off and on to compare with the game. Now: %s.",
        state.bypassed ? "off (the game's image)" : "on");

    const char* views[]{"Off", "Ray-traced geometry", "Depth comparison", "Ray-traced sun light", "Sun shadow comparison",
        "Shadow caster depth comparison", "Ray-traced ambient occlusion", "Ambient occlusion comparison", "Off-screen shadow check",
        "Ray-traced global illumination"};
    int view = static_cast<int>(state.view);
    ImGui::SetNextItemWidth(220 * scale);
    if (ImGui::Combo("Debug view", &view, views, static_cast<int>(std::size(views)))) select(static_cast<View>(view));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Depth comparison: green where the traced hit matches the game's depth, red where the trace hits something "
            "nearer, blue where it hits something farther or nothing.\nSun shadow comparison: yellow where the game and the trace are both "
            "lit, red where only the game is, green where only the trace is.\nShadow caster depth comparison: the depth comparison "
            "for the casters taken from the game's sun shadow map alone.\nAmbient occlusion comparison: the game's on the left, the "
            "traced on the right, both as lighting reads them.\nRay-traced global illumination: the bounced light alone, as it is added.");
    if (!state.failure.empty()) ImGui::TextWrapped("Ray tracing stopped: %s Change a setting to retry.", state.failure.c_str());
    if (state.view == View::Off && !state.features.sunShadows && !state.features.ambientOcclusion && !state.features.globalIllumination)
        return;

    ImGui::Text("Frames traced: %llu", static_cast<unsigned long long>(state.tracedFrames));
    ImGui::Text("G-buffer draws: %u, captured %u (%u alpha-tested)", state.basePassDraws, state.capturedDraws, state.maskedDraws);
    ImGui::Text("Shadow casters captured from the sun shadow maps: %u", state.casterDraws);
    ImGui::Text("Cached static draws: %u, %u traced off view; foliage instances: %u", state.cachedDraws, state.tracedCachedDraws,
        state.foliageInstances);
    ImGui::Text("Alpha-tested materials baked: %u; draws without coverage: %u", state.bakedMaterials, state.unbakedDraws);
    ImGui::Text("Skipped: %u decals, sky or screen passes, %u unknown shader, %u unsupported, %u over capacity", state.backgroundDraws,
        state.unknownShaderDraws, state.unsupportedDraws, state.overCapacityDraws);
    ImGui::Text("Triangles: %llu of %llu", static_cast<unsigned long long>(state.triangles), static_cast<unsigned long long>(state.capacity));
    if (state.sunFound) ImGui::Text("Sun direction: %.3f, %.3f, %.3f (%u shadow resolve draws)", state.sun[0], state.sun[1], state.sun[2],
        state.shadowResolveDraws);
    else ImGui::TextUnformatted("Sun direction: waiting for the game's sun shadow pass.");
    ImGui::Text("Traced sun shadows: %s", state.shadowsInjected ? "added to the game's" : "not in use this frame");
    ImGui::Text("Traced ambient occlusion: %s, GPU %.2f ms", state.ambientInjected ? "in place of the game's" : "not in use this frame",
        state.ambientMilliseconds);
    ImGui::Text("Traced global illumination: %s, GPU %.2f ms", state.bounceAdded ? "added to the game's lighting" : "not in use this frame",
        state.bounceMilliseconds);
    ImGui::Text("CPU: %.2f ms repeating draws", state.captureMilliseconds);
    ImGui::Text("GPU: %.2f ms copy, %.2f ms build, %.2f ms trace", state.copyMilliseconds, state.buildMilliseconds, state.traceMilliseconds);
}

void drawIndicator()
{
    const auto state = status();
    if (!state.toggledAt || GetTickCount64() - state.toggledAt > 2000) return;

    const char* text = state.bypassed ? "Ray tracing: OFF (game)" : "Ray tracing: ON";
    const ImVec2 corner{40, 40}, padding{12, 8};
    const auto size = ImGui::CalcTextSize(text);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(corner, ImVec2(corner.x + size.x + padding.x * 2, corner.y + size.y + padding.y * 2), IM_COL32(0, 0, 0, 180), 6);
    draw->AddText(ImVec2(corner.x + padding.x, corner.y + padding.y), state.bypassed ? IM_COL32(255, 160, 80, 255) : IM_COL32(120, 255, 140, 255), text);
}
}
