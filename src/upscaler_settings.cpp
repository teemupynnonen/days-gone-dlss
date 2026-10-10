#include "upscaler_settings.hpp"

#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <system_error>

namespace days_gone_dlss::upscaler
{
namespace
{
constexpr std::string_view QualityKeys[]{"off", "dlaa", "quality", "balanced", "performance", "ultra_performance", "custom"};
static_assert(std::size(QualityKeys) == static_cast<std::size_t>(Quality::Count));

std::string_view trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r");
    return begin == text.npos ? std::string_view{} : text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

template<class T> bool number(std::string_view text, T& result)
{
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
    result = value;
    return true;
}
}

std::string_view label(Quality quality)
{
    switch (quality)
    {
    case Quality::Off: return "Off (game TAA)";
    case Quality::Dlaa: return "DLAA";
    case Quality::Quality: return "Quality";
    case Quality::Balanced: return "Balanced";
    case Quality::Performance: return "Performance";
    case Quality::UltraPerformance: return "Ultra Performance";
    case Quality::Custom: return "Custom";
    default: return "Unknown";
    }
}

std::string_view label(Preset preset)
{
    switch (preset)
    {
    case Preset::Default: return "Default";
    case Preset::J: return "J (transformer, less ghosting)";
    case Preset::K: return "K (transformer)";
    case Preset::L: return "L (Ultra Performance model)";
    case Preset::M: return "M (Performance model)";
    default: return "Unknown";
    }
}

bool validPreset(int value)
{
    return value == 0 || (value >= static_cast<int>(Preset::J) && value <= static_cast<int>(Preset::M));
}

float nominalScale(Quality quality, float customScale)
{
    switch (quality)
    {
    case Quality::Dlaa: return 1.0f;
    case Quality::Quality: return 2.0f / 3.0f;
    case Quality::Balanced: return 0.58f;
    case Quality::Performance: return 0.5f;
    case Quality::UltraPerformance: return 1.0f / 3.0f;
    case Quality::Custom:
        return std::isfinite(customScale) ? std::clamp(customScale, Settings::MinScale, Settings::MaxScale) : Settings::DefaultCustomScale;
    default: return 1.0f;
    }
}

int jitterPhases(float scale)
{
    if (!std::isfinite(scale) || scale <= 0) return 8;
    // The engine stores the phase count in a byte; very low scales are capped well below that.
    return std::clamp(static_cast<int>(std::ceil(8.0f / (scale * scale))), 8, 128);
}

float mipBias(float scale, float offset)
{
    if (!std::isfinite(scale) || scale <= 0) scale = 1;
    if (!std::isfinite(offset)) offset = 0;
    return std::log2(std::min(scale, 1.0f)) + std::clamp(offset, Settings::MinMipBias, Settings::MaxMipBias);
}

Settings loadSettings(const std::filesystem::path& path)
{
    Settings settings;
    std::ifstream input(path);
    if (!input)
    {
        if (!std::filesystem::exists(path)) return settings;
        throw std::runtime_error("Cannot read upscaler settings.");
    }
    std::string line;
    while (std::getline(input, line))
    {
        const auto separator = line.find('=');
        if (separator == line.npos) continue;
        const auto key = trim(std::string_view(line).substr(0, separator));
        const auto value = trim(std::string_view(line).substr(separator + 1));
        if (value.empty()) continue;
        if (key == "quality")
        {
            const auto found = std::find(std::begin(QualityKeys), std::end(QualityKeys), value);
            if (found != std::end(QualityKeys)) settings.quality = static_cast<Quality>(found - std::begin(QualityKeys));
        }
        else if (key == "custom_scale")
        {
            float scale{};
            if (number(value, scale) && std::isfinite(scale))
                settings.customScale = std::clamp(scale, Settings::MinScale, Settings::MaxScale);
        }
        else if (key == "preset")
        {
            int preset{};
            if (number(value, preset) && validPreset(preset)) settings.preset = static_cast<Preset>(preset);
        }
        else if (key == "mip_bias_offset")
        {
            float offset{};
            if (number(value, offset) && std::isfinite(offset))
                settings.mipBiasOffset = std::clamp(offset, Settings::MinMipBias, Settings::MaxMipBias);
        }
        else if (key == "sharpness")
        {
            float sharpness{};
            if (number(value, sharpness) && std::isfinite(sharpness))
                settings.sharpness = std::clamp(sharpness, 0.0f, Settings::MaxSharpness);
        }
    }
    if (input.bad()) throw std::runtime_error("Cannot read upscaler settings.");
    return settings;
}

void saveSettings(const std::filesystem::path& path, const Settings& settings)
{
    if (settings.quality >= Quality::Count || !validPreset(static_cast<int>(settings.preset)))
        throw std::runtime_error("Cannot save an unknown upscaler mode.");
    if (!std::isfinite(settings.customScale) || settings.customScale < Settings::MinScale || settings.customScale > Settings::MaxScale
        || !std::isfinite(settings.mipBiasOffset) || settings.mipBiasOffset < Settings::MinMipBias
        || settings.mipBiasOffset > Settings::MaxMipBias || !std::isfinite(settings.sharpness) || settings.sharpness < 0
        || settings.sharpness > Settings::MaxSharpness)
        throw std::runtime_error("Cannot save an invalid upscaler scale, bias or sharpness.");
    auto temporary = path;
    temporary += L".tmp";
    try
    {
        std::ofstream output(temporary, std::ios::trunc);
        output.imbue(std::locale::classic());
        output << "quality=" << QualityKeys[static_cast<std::size_t>(settings.quality)]
            << "\ncustom_scale=" << std::setprecision(std::numeric_limits<float>::max_digits10) << settings.customScale
            << "\npreset=" << static_cast<int>(settings.preset)
            << "\nmip_bias_offset=" << settings.mipBiasOffset
            << "\nsharpness=" << settings.sharpness << '\n';
        output.close();
        if (!output) throw std::runtime_error("Cannot write upscaler settings.");
        // Keep the last complete file if writing or replacing it fails.
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot replace upscaler settings");
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
}
