#include "ambient_occlusion.hpp"
#include "shader_bytecode.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace days_gone_dlss::upscaler
{
namespace
{
struct LightingShader
{
    std::array<std::uint32_t, 4> checksum;
    unsigned textureSlot;
};

// Deferred-lighting permutations with the packed directional AO gather. These
// checksums restrict the change to Bend's directional AO reconstruction.
constexpr LightingShader LightingShaders[]{
    {{0xd6fc4671, 0x0f7a1932, 0x334f9bde, 0x518ed285}, 13},
    {{0x47beddd9, 0xf25900db, 0x8fa7e30f, 0xa18007cf}, 10},
    {{0xf34231ab, 0x6a9cd578, 0x090f6448, 0x7cff6090}, 12},
    {{0x35bd1857, 0x9d370f72, 0x3b573cf5, 0x82ef7121}, 15},
    {{0x4e056c81, 0xb4590112, 0xc7554f8c, 0xe42dca9c}, 13},
    {{0xa584d131, 0xe4a0a363, 0x6d981aeb, 0xd6cf8d9b}, 18},
    {{0x92ae1cbe, 0xd87f6c6f, 0x28bf342e, 0xe9c11b59}, 15},
    {{0x3bcd724f, 0xb2195871, 0x4333022d, 0x9f0c0ed6}, 17},
    {{0xcccca33d, 0xada71c43, 0xcc424696, 0xc731bd99}, 20},
    {{0x8fda4cdc, 0x1664cbe6, 0x4a0c8491, 0x1c2d9e7d}, 18},
    {{0xe47f5a93, 0xd5a271b6, 0xb3309829, 0xed3eb9dd}, 13},
    {{0x68597207, 0x95aaa1a2, 0x3f3fc8a7, 0xfbe8fab4}, 9},
    {{0x6793c38e, 0x09f7cd44, 0xccd435ea, 0x4de48fe0}, 12},
    {{0x128f1152, 0xa93669fc, 0xd4b0da04, 0xd1807725}, 14},
    {{0x496cf32f, 0x1825ff80, 0x48290440, 0x541a16f3}, 13},
    {{0xcc04d8a2, 0x910afd3a, 0x6dab9bb7, 0x4d5bf67b}, 12},
    {{0x77ebae4b, 0xd415a982, 0x185e6eb5, 0x1fbd0db0}, 8},
    {{0x9a8b5506, 0x7b4e5ec4, 0xdc896d22, 0x2c7466de}, 11},
    {{0x1982db4d, 0xb5f04ef0, 0x1b7ff4bc, 0x119da3f9}, 13},
    {{0xa6f32649, 0x081fcfdb, 0xad4f0306, 0x5124bdbd}, 12},
    {{0xdb119b71, 0xc6021b28, 0x1863312f, 0x05483f36}, 18},
    {{0xcfe9411f, 0xfad371a3, 0x143a800a, 0x21fe1c8d}, 14},
    {{0xe1276624, 0x383fc38b, 0x9702ac55, 0x1946eeef}, 17},
    {{0x2ed67d17, 0x237dcd3c, 0x9b1d1461, 0x787a07ce}, 19},
    {{0x50cc45f7, 0xee678e1d, 0xa080ab99, 0x81e53c0d}, 18},
    {{0x06c109b3, 0x04e0ba8b, 0xc5dfbaaf, 0xa809e3a5}, 17},
    {{0x4dc64b3a, 0xc6212856, 0xd5c233d3, 0xbfb8baae}, 13},
    {{0xd70518c5, 0x04d4b765, 0x3bdaed32, 0x135320fb}, 16},
    {{0xf27fad2d, 0xaebcc056, 0x989eae4c, 0xdc9218a4}, 18},
    {{0x5d15748d, 0xc4b5e8a2, 0x43c1cd0e, 0x50109de3}, 17},
};
}

AmbientOcclusionPatch makeFullResolutionAmbientOcclusion(const void* code, std::size_t size)
{
    if (!code || size < 20 || std::memcmp(code, "DXBC", 4)) return {};
    std::array<std::uint32_t, 4> checksum;
    std::memcpy(checksum.data(), static_cast<const unsigned char*>(code) + 4, 16);
    const auto shader = std::find_if(std::begin(LightingShaders), std::end(LightingShaders),
        [&](const LightingShader& candidate) { return candidate.checksum == checksum; });
    if (shader == std::end(LightingShaders)) return {};
    auto bytecode = readPixelBytecode(code, size);
    if (bytecode.words.empty()) return {};
    auto program = bytecode.program();
    std::size_t conversion{};
    for (std::size_t i = 2; i < program.size();)
    {
        const auto length = instructionLength(program.subspan(i));
        const auto remaining = program.subspan(i);
        // mul rN.xyzw, rN.xyzw, l(16,16,16,16); ftou rN.xyzw, rN.xyzw
        if (remaining.size() >= 15 && remaining[0] == 0x0a000038
            && remaining[1] == 0x001000f2 && remaining[3] == 0x00100e46
            && remaining[2] == remaining[4] && remaining[5] == 0x00004002
            && std::all_of(remaining.begin() + 6, remaining.begin() + 10, [](auto word) { return word == 0x41800000; })
            && remaining[10] == 0x0500001c && remaining[11] == remaining[1]
            && remaining[12] == remaining[2] && remaining[13] == remaining[3] && remaining[14] == remaining[2])
        {
            if (conversion) return {};
            conversion = i + 10;
        }
        i += length;
    }
    if (!conversion) return {};
    // Interpolated UVs can make a nominal weight of 1 slightly smaller than 1.
    // Truncation then drops the current pixel; foliage depth edges can reject all
    // remaining samples, producing fully unoccluded pixels in a 2x2 pattern.
    // Round the weights before integer conversion, retaining the original filter.
    std::array<std::uint32_t, 5> rounding;
    std::copy_n(program.begin() + static_cast<std::ptrdiff_t>(conversion), 5, rounding.begin());
    rounding[0] = 0x05000040; // round_ne rN.xyzw, rN.xyzw
    insertShaderInstruction(bytecode, conversion, rounding);
    updateDxbcChecksum(bytecode.words);
    return {std::move(bytecode.words), shader->textureSlot};
}
}
