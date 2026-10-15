#pragma once

#include "frame_generation.hpp"

#include <filesystem>
#include <string>

// Presents the game's Direct3D 11 frames through a Direct3D 12 swap chain, which DLSS Frame
// Generation needs. The game gets a stand-in swap chain whose back buffer is a texture shared with
// Direct3D 12; each present copies it into the real swap chain's back buffer, which Streamline
// (frame_generation.hpp) owns. ReShade's overlay runs on the stand-in, so it draws into the frame
// before the copy.
namespace days_gone_dlss::presenter
{
// The add-on's work for each game frame, on the presenting thread, before the frame leaves
// Direct3D 11. Returns what frame generation needs from the frame.
using FrameCallback = frame_generation::Inputs (*)();

// At add-on load, before the game creates its swap chain. Takes over the game's next swap chain when
// frame generation can run; Streamline is loaded from `directory`. Later calls do nothing.
void install(FrameCallback frame, const std::filesystem::path& directory) noexcept;
// The game presents through Direct3D 12.
bool active() noexcept;
// One line for the overlay: the route the game's frames take, or why frame generation did not start.
// Empty when install() was not called.
std::string status();
}
