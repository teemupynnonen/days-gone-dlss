#include "upscaler_settings.hpp"

#include <Windows.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
namespace fs = std::filesystem;
using namespace days_gone_dlss::upscaler;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool approximately(float actual, float expected) { return std::abs(actual - expected) < 1e-4f; }

void write(const fs::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    expect(static_cast<bool>(output), "Write the test fixture.");
}

void scales()
{
    expect(nominalScale(Quality::Dlaa, 0.5f) == 1.0f, "DLAA renders at output resolution.");
    expect(approximately(nominalScale(Quality::Quality, 0), 2.0f / 3.0f) && approximately(nominalScale(Quality::Balanced, 0), 0.58f)
        && nominalScale(Quality::Performance, 0) == 0.5f && approximately(nominalScale(Quality::UltraPerformance, 0), 1.0f / 3.0f),
        "Fixed modes use NVIDIA's standard ratios.");
    expect(nominalScale(Quality::Custom, 0.8f) == 0.8f, "Custom mode uses the chosen ratio.");
    expect(nominalScale(Quality::Custom, 0.1f) == Settings::MinScale && nominalScale(Quality::Custom, 2) == Settings::MaxScale,
        "Custom ratios stay within the range DLSS supports.");
    expect(nominalScale(Quality::Custom, std::numeric_limits<float>::quiet_NaN()) == Settings::DefaultCustomScale,
        "An invalid custom ratio cannot reach the renderer.");

    // DLSS asks for 8 * (output / render)^2 jitter phases.
    expect(jitterPhases(1.0f) == 8, "DLAA keeps the engine's 8-phase sequence.");
    expect(jitterPhases(2.0f / 3.0f) == 18 && jitterPhases(0.5f) == 32 && jitterPhases(1.0f / 3.0f) == 72,
        "Quality, Performance and Ultra Performance get their recommended phase counts.");
    expect(jitterPhases(0.01f) == 128 && jitterPhases(0) == 8 && jitterPhases(std::numeric_limits<float>::infinity()) == 8,
        "Phase counts stay within the engine's byte-sized sample index.");

    expect(mipBias(1.0f, 0) == 0 && mipBias(0.5f, 0) == -1 && approximately(mipBias(0.5f, -0.5f), -1.5f),
        "Texture bias follows log2 of the render scale plus the user's offset.");
    expect(mipBias(0.5f, -10) == -1 + Settings::MinMipBias && mipBias(2.0f, 0) == 0,
        "The offset is clamped and supersampling never adds a positive bias.");
}

void persistence(const fs::path& root)
{
    const auto path = root / L"DaysGoneDLSS.ini";
    expect(loadSettings(path) == Settings{}, "A first launch keeps the game's own anti-aliasing.");
    expect(!fs::exists(path), "Loading defaults must not create a settings file.");

    const Settings chosen{Quality::Custom, 0.625f, Preset::K, -0.25f};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores the mode, custom scale, preset and texture bias.");
    for (int i = 0; i < static_cast<int>(Quality::Count); ++i)
    {
        auto mode = chosen;
        mode.quality = static_cast<Quality>(i);
        saveSettings(path, mode);
        expect(loadSettings(path) == mode, "Every mode survives a restart.");
    }

    write(path, "# DLSS\r\nunknown=future\r\n quality = performance \r\npreset=13\r\nbroken\r\n");
    expect(loadSettings(path) == Settings{Quality::Performance, Settings::DefaultCustomScale, Preset::M, 0},
        "CRLF, whitespace and unknown keys keep the valid fields.");
    write(path, "quality=ludicrous\npreset=7\ncustom_scale=nan\nmip_bias_offset=5\n");
    expect(loadSettings(path) == Settings{Quality::Off, Settings::DefaultCustomScale, Preset::Default, Settings::MaxMipBias},
        "Unknown modes and reserved presets fall back to defaults; bias is clamped.");
    write(path, "custom_scale=0.05\n");
    expect(loadSettings(path).customScale == Settings::MinScale, "Clamp a custom scale below what DLSS supports.");

    for (const auto& invalid : {Settings{Quality::Custom, std::numeric_limits<float>::quiet_NaN()},
        Settings{Quality::Custom, 1.5f}, Settings{Quality::Quality, 0.75f, static_cast<Preset>(7)},
        Settings{Quality::Count}, Settings{Quality::Quality, 0.75f, Preset::Default, -3}})
    {
        saveSettings(path, chosen);
        bool rejected = false;
        try { saveSettings(path, invalid); }
        catch (const std::exception&) { rejected = true; }
        expect(rejected && loadSettings(path) == chosen, "Invalid settings cannot replace valid saved ones.");
    }
}
}

int main()
{
    const auto root = fs::temp_directory_path() / (L"days-gone-dlss-test-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64()));
    try
    {
        fs::create_directories(root);
        scales();
        persistence(root);
        fs::remove_all(root);
        std::cout << "Upscaler settings tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
