// Ray-tracing experiment sections of the ReShade overlay window, and its A/B indicator.
#include "ray_tracing.hpp"
#include "menu_style.hpp"

#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <format>
#include <iterator>

namespace days_gone_dlss::ray_tracing
{
namespace
{
using namespace menu;

// One coloured line on what the ray tracing is doing right now.
void drawStatus(const Status& state)
{
    const auto& features = state.features;
    const bool anyFeature = features.sunShadows || features.ambientOcclusion || features.globalIllumination;
    if (!state.failure.empty()) statusLine(Red, std::format("Ray tracing stopped: {} Change a setting to retry.", state.failure));
    else if (!anyFeature && state.view == View::Off) statusLine(Grey, "Ray tracing is off.");
    else if (state.bypassed) statusLine(Yellow, "Switched off with F9: the game's own image is shown.");
    else if (!state.tracedFrames) statusLine(Yellow, "Waiting for the first traced frame.");
    else statusLine(Green, "Ray tracing active. F9 switches it off to compare with the game.");
}
}

void drawMenu()
{
    const bool open = ImGui::CollapsingHeader("Ray tracing (experimental)");
    tooltip("Traces the geometry the game draws to its G-buffer and sun shadow map. A step toward path tracing;\n"
        "it costs performance and is not meant for play.");
    if (!open) return;

    const auto state = status();
    drawStatus(state);
    ImGui::Separator();

    auto features = state.features;
    if (ImGui::Checkbox("Ray-traced sun shadows", &features.sunShadows)) enable(features);
    tooltip("Adds sun shadows traced from the geometry on screen and the game's shadow casters around it to the game's own shadows.");

    ImGui::Indent();
    ImGui::BeginDisabled(!features.sunShadows);
    if (ImGui::Checkbox("Replace the game's soft tree shadows", &features.replaceCanopyShadows)) enable(features);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Leaves out the game's soft shadow under tree canopies, so only the traced trees and leaves shadow there.\n"
            "Needs the ray-traced sun shadows.");
    if (ImGui::Checkbox("Off-screen objects and trees", &features.offscreenGeometry)) enable(features);
    tooltip("Also traces cached static objects and copies of the trees around the camera, so their shadows stay when they\n"
        "leave the view. Off: only what is on screen and the game's shadow casters cast.");
    ImGui::Unindent();

    if (ImGui::Checkbox("Ray-traced ambient occlusion", &features.ambientOcclusion)) enable(features);
    tooltip("Replaces the game's screen-space ambient occlusion with occlusion traced from the same geometry, filtered by\n"
        "the game's own blur and accumulation.");

    ImGui::Indent();
    ImGui::BeginDisabled(!features.ambientOcclusion);
    if (ImGui::Checkbox("Half the rays", &features.halfAmbientRays)) enable(features);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Each pixel traces two of its four directions and takes the other two from its neighbour: about half the\n"
            "cost, a little more noise. Needs the ray-traced ambient occlusion.");
    ImGui::Unindent();
    if (ImGui::Checkbox("Ray-traced global illumination", &features.globalIllumination)) enable(features);
    tooltip("Adds sunlight bounced once off the same geometry to the game's lighting, accumulated over frames and filtered.");
}

void drawDiagnostics(float width)
{
    const auto state = status();
    const char* views[]{"Off", "Ray-traced geometry", "Depth comparison", "Ray-traced sun light", "Sun shadow comparison",
        "Shadow caster depth comparison", "Ray-traced ambient occlusion", "Ambient occlusion comparison", "Off-screen shadow check",
        "Ray-traced global illumination"};
    int view = static_cast<int>(state.view);
    ImGui::SetNextItemWidth(width);
    if (ImGui::Combo("Ray tracing view", &view, views, static_cast<int>(std::size(views)))) select(static_cast<View>(view));
    tooltip("Depth comparison: green where the traced hit matches the game's depth, red where the trace hits something\n"
        "nearer, blue where it hits something farther or nothing.\nSun shadow comparison: yellow where the game and the trace are both "
        "lit, red where only the game is, green where only the trace is.\nShadow caster depth comparison: the depth comparison "
        "for the casters taken from the game's sun shadow map alone.\nAmbient occlusion comparison: the game's on the left, the "
        "traced on the right, both as lighting reads them.\nRay-traced global illumination: the bounced light alone, as it is added.");

    const auto summary = std::format("Ray tracing: {} frames traced###rayTracing", state.tracedFrames);
    if (!ImGui::TreeNode(summary.c_str())) return;

    ImGui::Text("G-buffer draws: %u, captured %u (%u alpha-tested)", state.basePassDraws, state.capturedDraws, state.maskedDraws);
    ImGui::Text("Shadow casters captured from the sun shadow maps: %u; left to the view: %u, to the cache: %u", state.casterDraws,
        state.casterDrawsInView, state.casterDrawsInCache);
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
    ImGui::TreePop();
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
