#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace days_gone_dlss::upscaler
{
struct AmbientOcclusionPatch
{
    std::vector<std::uint32_t> bytecode;
    unsigned textureSlot{};
};

// Only the verified lighting shaders for build 19221447 are eligible. The caller
// must bind this variant only when its AO texture matches the lighting target size.
AmbientOcclusionPatch makeFullResolutionAmbientOcclusion(const void* code, std::size_t size);
}
