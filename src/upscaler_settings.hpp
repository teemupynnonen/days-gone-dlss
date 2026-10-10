#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace days_gone_dlss::upscaler
{
enum class Quality : std::uint8_t { Off, Dlaa, Quality, Balanced, Performance, UltraPerformance, Custom, Count };
// Values match NVSDK_NGX_DLSS_Hint_Render_Preset; Default lets the DLSS DLL choose per quality mode.
enum class Preset : std::uint8_t { Default = 0, J = 10, K = 11, L = 12, M = 13 };

struct Settings
{
    static constexpr float MinScale = 0.33f, MaxScale = 1.0f, DefaultCustomScale = 0.75f;
    static constexpr float MinMipBias = -2.0f, MaxMipBias = 1.0f;
    static constexpr float DefaultSharpness = 0.33f, MaxSharpness = 1.0f;
    Quality quality{Quality::Off};
    float customScale{DefaultCustomScale};
    Preset preset{Preset::Default};
    // Added to the automatic log2(render / output) texture LOD bias.
    float mipBiasOffset{};
    // RCAS strength on the DLSS output; 0 leaves it unsharpened.
    float sharpness{DefaultSharpness};

    bool operator==(const Settings&) const = default;
};

std::string_view label(Quality quality);
std::string_view label(Preset preset);
bool validPreset(int value);

// Render width divided by output width. DLSS's own optimal sizes take precedence when NGX is ready.
float nominalScale(Quality quality, float customScale);
// DLSS recommends at least 8 jitter phases, scaled by the pixel ratio: 8 * (output / render)^2.
int jitterPhases(float scale);
// Texture LOD bias that keeps output-resolution texture detail when rendering below it.
float mipBias(float scale, float offset);

// Missing files and invalid fields keep their defaults; I/O failures throw.
Settings loadSettings(const std::filesystem::path& path);
void saveSettings(const std::filesystem::path& path, const Settings& settings);
}
