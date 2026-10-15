#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace days_gone_dlss::upscaler
{
constexpr std::uint8_t nibble(char value) { return static_cast<std::uint8_t>(value <= '9' ? value - '0' : value - 'a' + 10); }

// The 16-byte DXBC header checksum (bytes 4 to 20 of a container) from its hex form.
constexpr std::array<std::uint8_t, 16> dxbcHash(const char (&hex)[33])
{
    std::array<std::uint8_t, 16> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint8_t>(nibble(hex[i * 2]) << 4 | nibble(hex[i * 2 + 1]));
    return result;
}

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
