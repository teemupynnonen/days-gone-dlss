#include "indirect_lighting.hpp"

#include <array>
#include <cstring>

namespace days_gone_dlss::upscaler
{
IndirectLightingPass indirectLightingPass(const void* code, std::size_t size)
{
    // FBendScreenSpaceILCS permutations from 1.07's _P patch pak global shader cache.
    constexpr std::array<std::uint32_t, 4> Gather{0x566fdc89, 0x958e1652, 0x23307ad8, 0xd9fad4a9};
    constexpr std::array<std::uint32_t, 4> HorizontalBlur{0xa1e19d23, 0x310ed4d2, 0x97620683, 0xe37f3bca};
    constexpr std::array<std::uint32_t, 4> VerticalBlur{0xb768a647, 0x88f01332, 0x67c81a06, 0x43dacfd2};
    constexpr std::array<std::uint32_t, 4> Downsample{0xefd6c957, 0x79be9ed7, 0x82c43c3c, 0xe3a6d5d4};
    if (!code || size < 20 || std::memcmp(code, "DXBC", 4)) return IndirectLightingPass::None;

    std::array<std::uint32_t, 4> checksum;
    std::memcpy(checksum.data(), static_cast<const unsigned char*>(code) + 4, 16);
    if (checksum == Gather) return IndirectLightingPass::Gather;
    if (checksum == HorizontalBlur) return IndirectLightingPass::HorizontalBlur;
    if (checksum == VerticalBlur) return IndirectLightingPass::VerticalBlur;
    if (checksum == Downsample) return IndirectLightingPass::Downsample;
    return IndirectLightingPass::None;
}
}
