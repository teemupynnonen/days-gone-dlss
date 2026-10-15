#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIAdapter;
struct IUnknown;

// DLSS Frame Generation through NVIDIA Streamline, on the presenter's Direct3D 12 swap chain
// (presenter.hpp). Streamline is loaded from the add-on's directory, and only when the setting is on.
namespace days_gone_dlss::frame_generation
{
// The engine's camera for a frame as FViewMatrices keeps it: row vectors, translated world.
struct Camera
{
    float projection[16]{};      // ProjMatrix, jittered
    float projectionJitter[2]{}; // What the jitter added to projection[8] and projection[9]
    float translatedView[16]{};  // TranslatedViewMatrix: translated world to view
    float origin[3]{};           // ViewOrigin, the camera's world position
};

// What frame generation needs from the frame about to be presented. The textures are Direct3D 11's,
// shared with Direct3D 12: depth and motion at render resolution from the buffer origin, motion in
// render pixels toward the previous frame, and the colour before the UI at output size.
struct Inputs
{
    // The frame's number, as the game thread and render thread marked it; 0 when no scene view
    // reached this present.
    std::uint32_t frame{};
    bool valid{}; // DLSS ran on this frame and its composite wrote the hudless colour
    ID3D11Texture2D* depth{};
    ID3D11Texture2D* motion{};
    ID3D11Texture2D* hudless{};
    unsigned renderWidth{}, renderHeight{};
    float jitter[2]{}; // In render pixels, +Y down, as DLSS takes it
    bool jitteredMotion{};
    bool reset{}; // A camera cut, or the previous frame is not the one before
    Camera camera;
};

// Before the game creates its swap chain: loads Streamline from `directory` and checks that DLSS
// Frame Generation runs on `adapter`. False, with the reason in status(), when it cannot.
bool start(const std::filesystem::path& directory, IDXGIAdapter* adapter) noexcept;
// Gives Streamline the native device the presenter made.
bool attach(ID3D12Device* device) noexcept;
// Streamline's proxy of a native device or DXGI factory, through which the presenter makes its
// queue and swap chain; and the object behind one of its proxies. Both add a reference.
IUnknown* upgrade(IUnknown* native) noexcept;
IUnknown* nativeOf(IUnknown* proxy) noexcept;
// Releases Streamline, before the devices it was given go.
void stop() noexcept;

// ---- Reflex: each frame's sleep and PC Latency markers, on the thread doing the work ----
// The game thread, around the engine's tick for `frame`. Reflex sleeps first, so the frame starts
// as late as the GPU allows and reads input then.
void beginSimulation(std::uint32_t frame) noexcept;
void endSimulation(std::uint32_t frame) noexcept;
// The render thread, as it starts drawing the frame's scene.
void beginRender(std::uint32_t frame) noexcept;

// ---- Each presented frame, on the presenting thread ----
// Records Streamline's copies of the inputs into `list`, which runs before the frame is presented.
// The resources are Direct3D 12's views of the inputs' textures, in the common state.
void tag(ID3D12GraphicsCommandList* list, const Inputs& inputs, ID3D12Resource* depth, ID3D12Resource* motion,
    ID3D12Resource* hudless) noexcept;
// Around the swap chain's Present: the frame's camera and mode, and the present markers.
void beginPresent(const Inputs& inputs) noexcept;
void endPresent(const Inputs& inputs) noexcept;

// ---- Settings UI ----
// Frames presented per rendered frame: below 2 turns generation off. `dynamic` lets DLSS Frame
// Generation choose them to reach the display's refresh rate. Applies at the next present while
// Streamline runs; a multiplier above the GPU's maximum is limited to it.
void select(unsigned multiplier, bool dynamic) noexcept;
// What the GPU supports, known once Streamline reports its state: zero until then.
struct Capabilities
{
    unsigned maxMultiplier{};
    bool dynamic{};
};
Capabilities capabilities() noexcept;
bool running() noexcept;
// One line for the overlay: generating, or why not.
std::string status();
}
