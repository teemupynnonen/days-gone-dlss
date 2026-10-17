#pragma once

#include <array>
#include <cstdint>

namespace days_gone_dlss::ray_tracing
{
constexpr unsigned BlueNoiseSize = 64;

// A BlueNoiseSize-square tile of blue noise that wraps seamlessly: each of its BlueNoiseSize² ranks
// once, as the 16-bit fraction (rank + 0.5) / BlueNoiseSize², with nearby texels far apart in rank.
// Rows from the top.
std::array<std::uint16_t, BlueNoiseSize * BlueNoiseSize> blueNoise();
}
