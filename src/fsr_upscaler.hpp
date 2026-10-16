#pragma once

#include <filesystem>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;

// AMD FSR upscaling in place of DLSS (upscaler.cpp). AMD's runtime runs on Direct3D 12 only, so a
// native Direct3D 12 device on the game's GPU upscales textures Direct3D 11 shares, and the two hand
// over through a shared fence. The runtime runs FSR 4 on Radeon RX 7000 and 9000 and FSR 3.1 on
// other GPUs.
namespace days_gone_dlss::fsr_upscaler
{
// One frame's inputs: Direct3D 11 textures created shared. All but the output are at render resolution
// from the buffer origin; FSR is set up for the sizes of the colour and output textures.
struct Inputs
{
    ID3D11Texture2D* color{};    // Linear: the square-root encoding squared, above 1 in HDR
    ID3D11Texture2D* depth{};    // Device depth, reversed with an infinite far plane
    ID3D11Texture2D* motion{};   // In render pixels toward the previous frame
    ID3D11Texture2D* reactive{}; // 1 where the game's TAA rejects history
    ID3D11Texture2D* output{};
    unsigned renderWidth{}, renderHeight{};
    float jitter[2]{};   // In render pixels, +Y down
    float frameTime{};   // Milliseconds since the previous frame
    float cameraNear{};  // In centimetres
    float verticalFov{}; // In radians
    bool hdr{}, reset{};
};

// On the thread executing Direct3D work, as everything below. Loads AMD's runtime from `directory`
// and makes the Direct3D 12 device on `device`'s GPU. Throws when FSR cannot run here.
void start(ID3D11Device* device, const std::filesystem::path& directory);
bool started() noexcept;
// Upscales at this point of the device's immediate context: Direct3D 12 waits for the work recorded so
// far, and the work recorded after waits for the output. Throws on failure.
void evaluate(const Inputs& inputs);
// Frees FSR and Direct3D 12, for another upscaler or the device's end.
void stop() noexcept;
// "FSR 4.1.1" or "FSR 3.1.5", once FSR has run; "FSR" before.
std::string name();
}
