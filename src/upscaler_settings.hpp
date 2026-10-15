#pragma once

#include "ray_tracing.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace days_gone_dlss::upscaler
{
enum class Quality : std::uint8_t { Off, Dlaa, Quality, Balanced, Performance, UltraPerformance, Custom, Count };
// Values match NVSDK_NGX_DLSS_Hint_Render_Preset; Default lets the DLSS DLL choose per quality mode.
enum class Preset : std::uint8_t { Default = 0, J = 10, K = 11, L = 12, M = 13 };
// Frames presented per rendered frame. Dynamic lets DLSS Frame Generation choose, to reach the
// display's refresh rate.
enum class FrameGeneration : std::uint8_t { Off, X2, X3, X4, X5, X6, Dynamic, Count };

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
    // 1.07's screen-space indirect lighting while DLSS runs; off renders as the game's lower presets do.
    bool indirectLighting{true};
    // DLSS Frame Generation. The game then presents through Direct3D 12, which is set up with its
    // swap chain at startup, so turning it on applies after a restart.
    FrameGeneration frameGeneration{FrameGeneration::Off};
    // The ray-tracing experiment's features, applied at startup on 1.13.
    ray_tracing::Features rayTracing;

    bool operator==(const Settings&) const = default;
};

std::string_view label(Quality quality);
std::string_view label(Preset preset);
std::string_view label(FrameGeneration mode);
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
