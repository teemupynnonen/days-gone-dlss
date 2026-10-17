#include "blue_noise.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace days_gone_dlss::ray_tracing
{
// Void-and-cluster's void filling (Ulichney 1993): texels are ranked one at a time, each the one
// farthest from those ranked before it, where the Gaussian energy the ranked texels spread over the
// wrapping tile is lowest. A tiny random energy breaks the ties among the first few, which would
// otherwise fall on a regular grid. A fixed seed gives the same tile every start.
std::array<std::uint16_t, BlueNoiseSize * BlueNoiseSize> blueNoise()
{
    constexpr int size = BlueNoiseSize, texels = size * size, radius = 8;
    constexpr float sigma = 1.5f;
    float kernel[2 * radius + 1][2 * radius + 1];
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x)
            kernel[y + radius][x + radius] = std::exp(-static_cast<float>(x * x + y * y) / (2 * sigma * sigma));

    std::mt19937 random(1);
    std::uniform_real_distribution<float> tiny(0, 1e-3f);
    std::vector<float> energy(texels);
    for (auto& value : energy) value = tiny(random);

    // A ranked texel's energy is infinite, so it is never chosen again.
    std::array<std::uint16_t, texels> ranks{};
    for (int rank = 0; rank < texels; ++rank)
    {
        const auto best = static_cast<int>(std::min_element(energy.begin(), energy.end()) - energy.begin());
        ranks[best] = static_cast<std::uint16_t>(rank * (65536 / texels) + 65536 / texels / 2);
        const int bestX = best % size, bestY = best / size;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x)
                energy[((bestY + y) & (size - 1)) * size + ((bestX + x) & (size - 1))] += kernel[y + radius][x + radius];
        energy[best] = std::numeric_limits<float>::infinity();
    }
    return ranks;
}
}
