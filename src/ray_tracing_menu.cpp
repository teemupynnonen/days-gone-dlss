// Ray-tracing experiment sections of the ReShade overlay window, and its A/B indicator.
#include "ray_tracing.hpp"
#include "menu_style.hpp"
#include "upscaler.hpp"

#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <algorithm>
#include <format>
#include <iterator>
#include <vector>

namespace days_gone_dlss::ray_tracing
{
namespace
{
using namespace menu;

const ImVec4 Blue{0.5f, 0.65f, 0.95f, 1}, Teal{0.4f, 0.8f, 0.75f, 1}, Orange{0.95f, 0.6f, 0.3f, 1}, Purple{0.75f, 0.55f, 0.95f, 1},
    Pink{0.95f, 0.5f, 0.7f, 1};

struct Part { const char* name; float milliseconds; };
struct CostRow { const char* name; ImVec4 colour; float milliseconds; std::vector<Part> parts; };

// What each feature costs per frame, and the render thread's share.
std::vector<CostRow> costRows(const Costs& c)
{
    return {
        {"Scene", Blue, c.scene(), {{"Placing vertices, copying depth", c.prepare}, {"Copy to Direct3D 12", c.copy}, {"View BLAS", c.viewBuild},
            {"Shadow casters BLAS", c.casterBuild}, {"Geometry cache builds", c.cacheBuild}, {"Foliage templates", c.templateBuild},
            {"TLAS", c.topBuild}, {"Direct3D 12 hand-over", c.sync}}},
        {"Sun shadows", Yellow, c.sunShadows(), {{"Sun rays (and debug views' primary rays)", c.sunRays}, {"Soft shadow filter", c.sunFilter},
            {"Adding to the game's shadow", c.sunInject}}},
        {"Ambient occlusion", Teal, c.ambientOcclusion(), {{"Rays", c.ambientRays}, {"Packing and the game's blur", c.ambientFilter}}},
        {"Global illumination", Orange, c.globalIllumination(), {{"Rays", c.bounceRays}, {"Accumulation and filter", c.bounceFilter},
            {"Adding to the scene", c.bounceAdd}}},
        {"Light shadows", Pink, c.lightShadows(), {{"Rays (shadows and bounce)", c.lightRays}, {"Direct3D 12 hand-over", c.lightSync},
            {"Soft shadow and bounce filters", c.lightFilter}, {"Constants and adding to the scene", c.lightAdd}}},
        {"Debug view", Grey, c.debugView, {}},
        {"CPU (render thread)", Purple, c.cpu(), {{"Repeating the game's draws", c.captureCpu},
            {"Preparing and submitting the trace", c.traceCpu}, {"  of which foliage", c.foliageCpu},
            {"  of which cache plans and instances", c.instancesCpu}, {"  of which Direct3D 11 flush", c.flushCpu},
            {"  of which record and submit", c.submitCpu}, {"  of which preparing", c.prepareCpu}, {"  of which other", c.otherTraceCpu()},
            {"  of which waiting for the GPU", c.gpuWaitCpu}}},
    };
}

// A horizontal bar filled to `fraction` of `width`.
void bar(float fraction, const ImVec4& colour, float width)
{
    const auto start = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeight();
    const ImVec2 top{start.x, start.y + height * 0.25f}, bottom{start.x + width, start.y + height * 0.8f};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(top, bottom, IM_COL32(255, 255, 255, 24), 2);
    const float filled = width * std::clamp(fraction, 0.0f, 1.0f);
    if (filled >= 1) draw->AddRectFilled(top, ImVec2(start.x + filled, bottom.y), ImGui::ColorConvertFloat4ToU32(colour), 2);
    ImGui::Dummy(ImVec2(width, height));
}

void rightAligned(const std::string& text, bool dim = false)
{
    const float offset = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text.c_str()).x;
    if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
    if (dim) ImGui::TextDisabled("%s", text.c_str());
    else ImGui::TextUnformatted(text.c_str());
}

// One row per feature: its bar against the costliest, its milliseconds and its share of the frame.
// With details, each opens to its parts.
void drawCostTable(const Costs& costs, bool details)
{
    const auto rows = costRows(costs);
    float largest = 0.01f;
    for (const auto& row : rows) largest = std::max(largest, row.milliseconds);
    const auto share = [&](float milliseconds) { return costs.frame > 0 ? std::format("{:.0f}%", milliseconds / costs.frame * 100) : std::string(); };
    if (!ImGui::BeginTable("costs", 4, ImGuiTableFlags_SizingFixedFit)) return;

    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
    ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
    ImGui::TableSetupColumn("frame", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.5f);
    for (const auto& row : rows)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, row.colour);
        bool open{};
        if (details)
        {
            const auto flags = ImGuiTreeNodeFlags_SpanAllColumns | (row.parts.empty() ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen : 0);
            open = ImGui::TreeNodeEx(row.name, flags) && !row.parts.empty();
        }
        else ImGui::TextUnformatted(row.name);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        bar(row.milliseconds / largest, row.colour, ImGui::GetFontSize() * 7);
        ImGui::TableNextColumn();
        rightAligned(std::format("{:.2f}", row.milliseconds));
        ImGui::TableNextColumn();
        rightAligned(share(row.milliseconds), true);
        if (!open) continue;

        for (const auto& part : row.parts)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", part.name);
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            rightAligned(std::format("{:.2f}", part.milliseconds), true);
        }
        ImGui::TreePop();
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("GPU total");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    rightAligned(std::format("{:.2f}", costs.gpu()));
    ImGui::TableNextColumn();
    rightAligned(share(costs.gpu()), true);
    ImGui::EndTable();
}

// The frame above the table, and what the ray tracing is doing if not the usual.
void drawHeadline(const Status& state)
{
    const auto& c = state.costs;
    ImGui::Text("Frame %.2f ms  (%.0f fps)", c.frame, c.frame > 0 ? 1000 / c.frame : 0.0f);
    if (state.measurement.running)
        statusLine(Yellow, std::format("Measuring the real cost: step {} of {}. Hold still.", state.measurement.step + 1, state.measurement.stepCount));
    else if (state.bypassed) statusLine(Yellow, "Paused with F9: nothing is traced.");
}

// What the last measurement found each feature really costs, everything included.
void drawRealCost(const CostMeasurement& m)
{
    if (m.steps.size() < 2)
    {
        if (!m.running) ImGui::TextDisabled("Real cost: not measured yet.");
        return;
    }

    float largest = 0.01f;
    for (std::size_t i = 1; i < m.steps.size(); ++i) largest = std::max(largest, m.steps[i - 1].frame - m.steps[i].frame);
    const auto minutes = (GetTickCount64() - m.finishedAt) / 60000;
    const auto age = minutes ? std::format("{} min ago", minutes) : std::string("just now");
    ImGui::Text("Real cost");
    ImGui::SameLine();
    ImGui::TextDisabled("(ms saved by leaving out, %s)", age.c_str());
    if (!ImGui::BeginTable("realCost", 3, ImGuiTableFlags_SizingFixedFit)) return;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("Frame with everything on");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    rightAligned(std::format("{:.2f}", m.steps.front().frame), true);

    for (std::size_t i = 1; i < m.steps.size(); ++i)
    {
        const auto saved = m.steps[i - 1].frame - m.steps[i].frame;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(m.steps[i].removed.c_str());
        ImGui::TableNextColumn();
        bar(saved / largest, Red, ImGui::GetFontSize() * 7);
        ImGui::TableNextColumn();
        rightAligned(std::format("{:.2f}", saved));
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("All of ray tracing");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    rightAligned(std::format("{:.2f}", m.steps.front().frame - m.steps.back().frame));
    ImGui::EndTable();
}

// The work behind the scene's cost, in two short lines.
void drawWork(const Status& state)
{
    ImGui::TextDisabled("Rebuilt %.2f M triangles from %u draws", static_cast<double>(state.triangles) / 1e6,
        state.capturedDraws + state.casterDraws);
    ImGui::TextDisabled("Tree copies %u, new cache entries %u", state.foliageInstances, state.newCacheEntries);
}

// One coloured line on what the ray tracing is doing right now.
void drawStatus(const Status& state)
{
    const auto& features = state.features;
    const bool anyFeature = features.sunShadows || features.ambientOcclusion || features.globalIllumination || features.lightShadows;
    if (!state.failure.empty()) statusLine(Red, std::format("Ray tracing stopped: {} Change a setting to retry.", state.failure));
    else if (!anyFeature && state.view == View::Off) statusLine(Grey, "Ray tracing is off.");
    else if (state.bypassed) statusLine(Yellow, "Switched off with F9: the game's own image is shown.");
    else if (!state.tracedFrames) statusLine(Yellow, "Waiting for the first traced frame.");
    else statusLine(Green, "Ray tracing active. F9 switches it off to compare with the game.");
}

// Applies the features and keeps them in DaysGoneDLSS.ini for the next start.
void save(const Features& features)
{
    enable(features);
    auto wanted = upscaler::currentSettings();
    wanted.rayTracing = features;
    upscaler::applySettings(wanted);
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
    if (ImGui::Checkbox("Ray-traced sun shadows", &features.sunShadows)) save(features);
    tooltip("Adds sun shadows traced from the geometry on screen and the game's shadow casters around it to the game's own shadows.");

    ImGui::Indent();
    ImGui::BeginDisabled(!features.sunShadows);
    if (ImGui::Checkbox("Replace the game's soft tree shadows", &features.replaceCanopyShadows)) save(features);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Leaves out the game's soft shadow under tree canopies, so only the traced trees and leaves shadow there.\n"
            "Needs the ray-traced sun shadows.");
    ImGui::BeginDisabled(!features.sunShadows);
    if (ImGui::Checkbox("Soft sun shadows", &features.softSunShadows)) save(features);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Traces toward points across the sun's or moon's disc, so shadows soften away from what casts them, as\n"
            "the light shadows' soft option does. Filtered over frames and neighbouring pixels. Needs the ray-traced sun shadows.");
    if (ImGui::Checkbox("Off-screen objects and trees", &features.offscreenGeometry)) save(features);
    tooltip("Also traces cached static objects and copies of the trees around the camera, so their shadows stay when they\n"
        "leave the view. Off: only what is on screen and the game's shadow casters cast.");
    ImGui::Unindent();

    if (ImGui::Checkbox("Ray-traced ambient occlusion", &features.ambientOcclusion)) save(features);
    tooltip("Traces ambient occlusion from the same geometry and keeps the darker of it and the game's screen-space\n"
        "occlusion, filtered by the game's own blur and accumulation.");

    ImGui::Indent();
    ImGui::BeginDisabled(!features.ambientOcclusion);
    if (ImGui::Checkbox("Half the rays", &features.halfAmbientRays)) save(features);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Each pixel traces two of its four directions and takes the other two from its neighbour: about half the\n"
            "cost, a little more noise. Needs the ray-traced ambient occlusion.");
    ImGui::Unindent();
    if (ImGui::Checkbox("Ray-traced global illumination", &features.globalIllumination)) save(features);
    tooltip("Adds sunlight bounced once off the same geometry to the game's lighting, accumulated over frames and filtered.");
    if (ImGui::Checkbox("Ray-traced light shadows", &features.lightShadows)) save(features);
    tooltip("Traces the shadows of the game's lamps, fires and flashlight from the same geometry in place of their shadow\n"
        "maps. Lights the game draws without shadows get them too. Up to 16 lights a frame; the rest keep the game's.");

    ImGui::Indent();
    ImGui::BeginDisabled(!features.lightShadows);
    if (ImGui::Checkbox("Soft shadows", &features.softLightShadows)) save(features);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Traces toward points across each light's size, so shadows soften away from what casts them. Lights the\n"
            "game gives no size get one: fires and bulbs by their reach, lamps 5 cm. Filtered over a few frames and\n"
            "neighbouring pixels. Needs the ray-traced light shadows.");
    ImGui::BeginDisabled(!features.lightShadows);
    if (ImGui::Checkbox("Bounce light", &features.lightBounce)) save(features);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Adds the lights' light bounced once off the same geometry, accumulated over frames and filtered, so\n"
            "their shadows fill in as in a real room. Needs the ray-traced light shadows.");
    ImGui::Unindent();
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
        "traced and the game's combined on the right, both as lighting reads them.\nRay-traced global illumination: the bounced "
        "light alone, as it is added.");

    bool overlay = state.performanceOverlay;
    if (ImGui::Checkbox("Show the costs over the game", &overlay)) showPerformance(overlay);
    tooltip("Keeps the cost table in the top right corner, also with this window closed.");
    ImGui::SameLine();
    ImGui::BeginDisabled(state.measurement.running);
    if (ImGui::Button("Measure the real cost")) measureCosts();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Times the frame with everything on, then leaves out one feature at a time, then the scene, then pauses ray\n"
            "tracing: about 4 seconds a step. The differences include what the timed passes miss, such as the repeated draws\n"
            "inside the game's passes. Hold still (Photo Mode) for steady numbers; your settings come back at the end.");

    ImGui::Spacing();
    drawHeadline(state);
    drawCostTable(state.costs, true);
    drawWork(state);
    ImGui::Spacing();
    drawRealCost(state.measurement);

    const auto summary = std::format("Capture statistics: {} frames traced###rayTracing", state.tracedFrames);
    if (!ImGui::TreeNode(summary.c_str())) return;

    ImGui::Text("G-buffer draws: %u, captured %u (%u alpha-tested)", state.basePassDraws, state.capturedDraws, state.maskedDraws);
    ImGui::Text("Shadow casters captured from the sun shadow maps: %u; left to the view: %u, to the cache: %u", state.casterDraws,
        state.casterDrawsInView, state.casterDrawsInCache);
    ImGui::Text("Cached static draws: %u, %u traced off view; foliage instances: %u", state.cachedDraws, state.tracedCachedDraws,
        state.foliageInstances);
    ImGui::Text("Alpha-tested materials baked: %u; draws without coverage: %u", state.bakedMaterials, state.unbakedDraws);
    ImGui::Text("Skipped: %u decals, sky or screen passes, %u unknown shader, %u unsupported, %u over capacity", state.backgroundDraws,
        state.unknownShaderDraws, state.unsupportedDraws, state.overCapacityDraws);
    ImGui::Text("Triangles: %llu of %llu (view %llu, shadow casters %llu)", static_cast<unsigned long long>(state.triangles),
        static_cast<unsigned long long>(state.capacity), static_cast<unsigned long long>(state.viewTriangles),
        static_cast<unsigned long long>(state.casterTriangles));
    if (state.sunFound) ImGui::Text("Sun direction: %.3f, %.3f, %.3f (%u shadow resolve draws)", state.sun[0], state.sun[1], state.sun[2],
        state.shadowResolveDraws);
    else ImGui::TextUnformatted("Sun direction: waiting for the game's sun shadow pass.");
    ImGui::Text("Traced sun shadows: %s", state.shadowsInjected ? "added to the game's" : "not in use this frame");
    ImGui::Text("Traced ambient occlusion: %s", state.ambientInjected ? "combined with the game's" : "not in use this frame");
    ImGui::Text("Traced global illumination: %s", state.bounceAdded ? "added to the game's lighting" : "not in use this frame");
    ImGui::Text("Local lights: %u traced, %u left to the game", state.tracedLights, state.gameLights);
    ImGui::TreePop();
}

void drawIndicator()
{
    const auto state = status();
    if (state.performanceOverlay)
    {
        const auto& display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x - 24, 24), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.75f);
        constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("Ray tracing costs###rayTracingCosts", nullptr, flags))
        {
            drawHeadline(state);
            drawCostTable(state.costs, false);
            drawWork(state);
            drawRealCost(state.measurement);
        }
        ImGui::End();
    }
    if (!state.toggledAt || GetTickCount64() - state.toggledAt > 2000) return;

    const char* text = state.bypassed ? "Ray tracing: OFF (game)" : "Ray tracing: ON";
    const ImVec2 corner{40, 40}, padding{12, 8};
    const auto size = ImGui::CalcTextSize(text);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(corner, ImVec2(corner.x + size.x + padding.x * 2, corner.y + size.y + padding.y * 2), IM_COL32(0, 0, 0, 180), 6);
    draw->AddText(ImVec2(corner.x + padding.x, corner.y + padding.y), state.bypassed ? IM_COL32(255, 160, 80, 255) : IM_COL32(120, 255, 140, 255), text);
}
}
