#pragma once

#include <cstddef>
#include <cstdint>

namespace days_gone_dlss::upscaler
{
// 1.07's screen-space indirect lighting passes in frame order: the gather (<0>) traces random rays,
// two blurs (<3>, <4>) denoise them and write the result the lighting reads (the second also blends
// in history), and after lighting the downsample (<2>) prepares the next frame's gather input.
enum class IndirectLightingPass : std::uint8_t { None, Gather, HorizontalBlur, VerticalBlur, Downsample };
IndirectLightingPass indirectLightingPass(const void* code, std::size_t size);
}
