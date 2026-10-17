#include "blue_noise.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace days_gone_dlss::ray_tracing;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Blue noise has little low-frequency energy: the means of small blocks stay close to one half. For
// white noise a 4 by 4 block's mean varies by 1 / sqrt(12) / 4, about 0.072.
void blue()
{
    const auto noise = blueNoise();
    constexpr unsigned texels = BlueNoiseSize * BlueNoiseSize, block = 4;
    std::vector<bool> seen(texels);
    for (const auto value : noise)
    {
        const auto rank = value / (65536 / texels);
        expect(value % (65536 / texels) == 65536 / texels / 2 && !seen[rank], "Each rank appears once, at the middle of its step.");
        seen[rank] = true;
    }

    double squares = 0;
    for (unsigned blockY = 0; blockY < BlueNoiseSize; blockY += block)
        for (unsigned blockX = 0; blockX < BlueNoiseSize; blockX += block)
        {
            double sum = 0;
            for (unsigned y = 0; y < block; ++y)
                for (unsigned x = 0; x < block; ++x) sum += noise[(blockY + y) * BlueNoiseSize + blockX + x] / 65536.0;
            squares += std::pow(sum / (block * block) - 0.5, 2);
        }
    const auto deviation = std::sqrt(squares / (texels / (block * block)));
    std::cout << "4x4 block means vary by " << deviation << " (white noise: 0.072)\n";
    expect(deviation < 0.036, "Blocks of blue noise average out far better than white noise's.");
}
}

int main()
{
    try
    {
        blue();
        std::cout << "Blue noise tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
