#pragma once

#include "frame_generation.hpp"

#include <filesystem>
#include <string>

// Presents the game's Direct3D 11 frames through a Direct3D 12 swap chain, which DLSS and FSR frame
// generation need. The game gets a stand-in swap chain whose back buffer is a texture shared with
// Direct3D 12; each present copies it into the real swap chain's back buffer, which Streamline
// (frame_generation.hpp) or AMD's swap chain (fsr_frame_generation.hpp) owns. ReShade wraps the
// stand-in as it would the game's own swap chain, so its overlay draws into the frame before the copy
// and add-ons see a Direct3D 11 game.
namespace days_gone_dlss::presenter
{
// Whose frame generation presents the frames.
enum class Route { Streamline, Fsr };

// At add-on load, before the game creates its swap chain. Takes over the game's next swap chain when
// frame generation can run on `route`, whose runtime is loaded from `directory`. Later calls do nothing.
void install(const std::filesystem::path& directory, Route route) noexcept;
// What frame generation needs from the frame being presented, from the add-on's ReShade present event,
// which comes just before the stand-in's Present.
void submit(const frame_generation::Inputs& inputs) noexcept;
// The game presents through Direct3D 12, on route().
bool active() noexcept;
Route route() noexcept;
// One line for the overlay: the route the game's frames take, or why frame generation did not start.
// Empty when install() was not called.
std::string status();
}
