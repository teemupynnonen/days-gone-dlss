#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace days_gone_dlss::upscaler
{
struct PixelBytecode
{
    std::vector<std::uint32_t> words;
    std::size_t offset{}, count{};
    std::span<std::uint32_t> program() { return std::span(words).subspan(offset, count); }
};

// Copy and validate a DXBC container containing one ps_5_0 program.
PixelBytecode readPixelBytecode(const void* code, std::size_t size);
std::size_t instructionLength(std::span<const std::uint32_t> words);
void updateDxbcChecksum(std::vector<std::uint32_t>& words);
void insertShaderInstruction(PixelBytecode& bytecode, std::size_t position, std::span<const std::uint32_t> instruction);
}
