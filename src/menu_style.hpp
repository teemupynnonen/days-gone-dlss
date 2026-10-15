#pragma once

#include <imgui.h>
#include <string>

// What the overlay window's sections share, so they read as one panel. The including file also
// includes reshade.hpp, which routes the ImGui calls through ReShade's overlay.
namespace days_gone_dlss::menu
{
inline const ImVec4 Grey{0.6f, 0.6f, 0.6f, 1}, Green{0.45f, 0.85f, 0.45f, 1}, Yellow{0.95f, 0.8f, 0.35f, 1}, Red{0.95f, 0.4f, 0.4f, 1};

inline void tooltip(const char* text)
{
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

inline void statusLine(const ImVec4& colour, const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}
}
