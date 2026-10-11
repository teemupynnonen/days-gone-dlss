#include "shader_bytecode.hpp"

#include <cstring>
#include <limits>

// Microsoft's DXBC container checksum implementation, downloaded and pinned by CMake.
void ComputeHashRetail(const unsigned char* data, unsigned int size, unsigned char* hash);

namespace days_gone_dlss::upscaler
{
std::size_t instructionLength(std::span<const std::uint32_t> words)
{
    if (words.empty()) return 0;
    constexpr std::uint32_t CustomData = 53;
    const auto length = (words[0] & 0x7ff) == CustomData
        ? (words.size() >= 2 ? words[1] : 0) : (words[0] >> 24) & 0x7f;
    return length && length <= words.size() ? length : 0;
}

PixelBytecode readPixelBytecode(const void* code, std::size_t size)
{
    if (!code || size < 36 || size % 4 || size > std::numeric_limits<unsigned int>::max()) return {};
    PixelBytecode result;
    auto& words = result.words;
    words.resize(size / 4);
    std::memcpy(words.data(), code, size);
    if (words[0] != 0x43425844 || words[5] != 1 || words[6] != size || words[7] > words.size() - 8) return {};
    for (std::size_t chunk = 0; chunk < words[7]; ++chunk)
    {
        const auto offset = words[8 + chunk];
        if (offset % 4 || offset < (8 + words[7]) * 4 || offset > size - 8) return {};
        const auto start = offset / 4;
        const auto bytes = words[start + 1];
        if (bytes > size - offset - 8) return {};
        if (words[start] != 0x52444853 && words[start] != 0x58454853) continue; // SHDR / SHEX
        if (result.count || bytes < 8 || bytes % 4) return {};
        result.offset = start + 2;
        result.count = bytes / 4;
        const auto program = result.program();
        if (program[0] != 0x50 || program[1] != program.size()) return {};
        for (std::size_t i = 2; i < program.size();)
        {
            const auto length = instructionLength(program.subspan(i));
            if (!length) return {};
            i += length;
        }
    }
    return result.count ? result : PixelBytecode{};
}

void updateDxbcChecksum(std::vector<std::uint32_t>& words)
{
    auto* bytes = reinterpret_cast<unsigned char*>(words.data());
    ComputeHashRetail(bytes + 20, static_cast<unsigned int>(words.size() * 4 - 20), bytes + 4);
}

void insertShaderInstruction(PixelBytecode& bytecode, std::size_t position, std::span<const std::uint32_t> instruction)
{
    auto& words = bytecode.words;
    const auto insertion = bytecode.offset + position;
    const auto bytes = static_cast<std::uint32_t>(instruction.size() * 4);
    words.insert(words.begin() + static_cast<std::ptrdiff_t>(insertion), instruction.begin(), instruction.end());
    words[6] += bytes;
    for (std::size_t chunk = 0; chunk < words[7]; ++chunk)
        if (words[8 + chunk] >= insertion * 4) words[8 + chunk] += bytes;
    words[bytecode.offset - 1] += bytes;
    words[bytecode.offset + 1] += static_cast<std::uint32_t>(instruction.size());
    bytecode.count += instruction.size();
}
}
