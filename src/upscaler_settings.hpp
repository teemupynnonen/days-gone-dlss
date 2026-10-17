#pragma once

#include "ray_tracing.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace days_gone_dlss::upscaler
{
// NVIDIA DLSS, or AMD FSR: FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 on other GPUs.
enum class Upscaler : std::uint8_t { Dlss, Fsr, Count };
// Dlaa is FSR's Native AA.
enum class Quality : std::uint8_t { Off, Dlaa, Quality, Balanced, Performance, UltraPerformance, Custom, Count };
// Values match NVSDK_NGX_DLSS_Hint_Render_Preset; Default lets the DLSS DLL choose per quality mode.
enum class Preset : std::uint8_t { Default = 0, J = 10, K = 11, L = 12, M = 13 };
// Frames presented per rendered frame, by DLSS Frame Generation up to Dynamic, which chooses them to
// reach the display's refresh rate. Fsr is FSR frame generation, 2x on any GPU.
enum class FrameGeneration : std::uint8_t { Off, X2, X3, X4, X5, X6, Dynamic, Fsr, Count };

struct Settings
{
    static constexpr float MinScale = 0.33f, MaxScale = 1.0f, DefaultCustomScale = 0.75f;
    static constexpr float MinMipBias = -2.0f, MaxMipBias = 1.0f;
    static constexpr float DefaultSharpness = 0.33f, MaxSharpness = 1.0f;
    Upscaler upscaler{Upscaler::Dlss};
    Quality quality{Quality::Off};
    float customScale{DefaultCustomScale};
    Preset preset{Preset::Default};
    // Added to the automatic log2(render / output) texture LOD bias.
    float mipBiasOffset{};
    // RCAS strength on the upscaled output; 0 leaves it unsharpened.
    float sharpness{DefaultSharpness};
    // 1.07's screen-space indirect lighting while upscaling runs; off renders as the game's lower presets do.
    bool indirectLighting{true};
    // The game then presents through Direct3D 12, which is set up with its swap chain at startup, so
    // turning frame generation on, or switching between DLSS and FSR, applies after a restart.
    FrameGeneration frameGeneration{FrameGeneration::Off};
    // The ray-tracing experiment's features on 1.13. It prepares the game's shaders as they are created,
    // so it only runs when one is on at startup, and turning it on applies after a restart.
    ray_tracing::Features rayTracing;

    bool operator==(const Settings&) const = default;
};

std::string_view label(Upscaler upscaler);
std::string_view label(Quality quality, Upscaler upscaler);
std::string_view label(Preset preset);
std::string_view label(FrameGeneration mode);
bool validPreset(int value);

// Render width divided by output width. DLSS's own optimal sizes take precedence when NGX is ready.
float nominalScale(Upscaler upscaler, Quality quality, float customScale);
// DLSS and FSR recommend at least 8 jitter phases, scaled by the pixel ratio: 8 * (output / render)^2.
int jitterPhases(float scale);
// Texture LOD bias that keeps output-resolution texture detail when rendering below it.
float mipBias(float scale, float offset);

// Missing files and invalid fields keep their defaults; I/O failures throw.
Settings loadSettings(const std::filesystem::path& path);
void saveSettings(const std::filesystem::path& path, const Settings& settings);
}
