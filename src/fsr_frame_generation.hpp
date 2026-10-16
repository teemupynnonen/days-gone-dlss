#pragma once

#include "frame_generation.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <filesystem>
#include <string>

// AMD FSR frame generation on the presenter's Direct3D 12 route (presenter.hpp), in place of DLSS
// Frame Generation: one frame generated between each pair the game renders, on any GPU. AMD's swap
// chain presents and paces the frames, and generates on the queue it presents from. The runtime runs
// FSR's machine-learning frame generation on Radeon RX 9000 and FSR 3.1 on other GPUs.
namespace days_gone_dlss::fsr_frame_generation
{
// Before the game creates its swap chain: loads AMD's runtime from `directory`. False, with the
// reason in status(), when it cannot.
bool start(const std::filesystem::path& directory) noexcept;
// AMD's swap chain for the game's window, presenting from `queue`; the caller owns the reference
// returned. Throws on failure.
IDXGISwapChain4* createSwapChain(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen);
// Each presented frame, on the presenting thread, before the swap chain's Present. Records into
// `list`, which runs on the swap chain's queue before the frame is presented, a copy of the hudless
// colour and FSR's preparation of depth and motion, then sets up generation for the frame. The
// resources are Direct3D 12's views of the inputs' textures, in the common state; `hdr` when the
// swap chain presents HDR.
void record(ID3D12GraphicsCommandList* list, const frame_generation::Inputs& inputs, ID3D12Resource* depth, ID3D12Resource* motion,
    ID3D12Resource* hudless, bool hdr) noexcept;
// Before the swap chain's buffers change; generation starts again with the next frame.
void release() noexcept;
// Before the swap chain goes.
void stop() noexcept;

// ---- Settings UI ----
// Applies at the next present while FSR's swap chain runs.
void select(bool on) noexcept;
bool running() noexcept;
// One line for the overlay: generating, or why not.
std::string status();
}
