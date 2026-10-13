#include "alpha_test.hpp"
#include "shader_bytecode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <span>

namespace days_gone_dlss::ray_tracing
{
namespace
{
// d3d11tokenizedprogramformat.hpp
constexpr std::uint32_t Discard = 13, CustomData = 53, ResourceDeclaration = 88;
constexpr std::uint32_t TempOperand = 0, InputOperand = 1, Immediate32 = 4, Immediate64 = 5;
constexpr std::uint32_t PositionValue = 1;

struct Operand
{
    std::uint32_t type{}, index{}, components{};
    std::array<std::uint8_t, 4> swizzle{0, 1, 2, 3}; // Source component read for each destination component
    std::uint8_t mask{0xf};                         // Destination components written
};

struct Instruction
{
    std::uint32_t opcode{};
    std::vector<Operand> operands;
};

bool declaration(std::uint32_t opcode)
{
    return (opcode >= 88 && opcode <= 106) || (opcode >= 143 && opcode <= 162) || opcode == 207;
}

std::size_t destinations(std::uint32_t opcode)
{
    switch (opcode)
    {
    case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case Discard: case 18: case 19: case 20: case 21:
    case 22: case 23: case 31: case 44: case 48: case 58: case 62: case 63: case 76: case 190: case 208: case 209:
        return 0;
    case 38: case 77: case 78: case 81: case 132: case 133: case 142:
        return 2;
    default:
        return 1;
    }
}

// Texture reads whose first source is the coordinate: sample, sample_c, sample_c_lz, sample_l,
// sample_d, sample_b, lod, gather4 and its comparison and offset forms.
bool sampling(std::uint32_t opcode)
{
    return (opcode >= 69 && opcode <= 74) || opcode == 108 || opcode == 109 || (opcode >= 126 && opcode <= 128);
}

// Instructions whose every source component can feed every destination component.
bool wholeVector(std::uint32_t opcode)
{
    return (opcode >= 15 && opcode <= 17) || sampling(opcode) || opcode == 45 || opcode == 46 || opcode == 61 || opcode >= 163;
}

bool readOperand(std::span<const std::uint32_t> words, std::size_t& i, Operand& operand)
{
    if (i >= words.size()) return false;

    const auto token = words[i++];
    const auto count = token & 3;
    operand.components = count == 0 ? 0 : count == 1 ? 1 : 4;
    if (count == 2)
    {
        const auto mode = (token >> 2) & 3;
        if (mode == 0) operand.mask = static_cast<std::uint8_t>((token >> 4) & 0xf);
        else if (mode == 1)
            for (int c = 0; c < 4; ++c) operand.swizzle[c] = static_cast<std::uint8_t>((token >> (4 + 2 * c)) & 3);
        else
        {
            const auto select = static_cast<std::uint8_t>((token >> 4) & 3);
            operand.swizzle = {select, select, select, select};
            operand.mask = static_cast<std::uint8_t>(1u << select);
        }
    }
    operand.type = (token >> 12) & 0xff;
    for (auto extended = token; extended & 0x80000000u;)
    {
        if (i >= words.size()) return false;
        extended = words[i++];
    }
    if (operand.type == Immediate32 || operand.type == Immediate64)
    {
        i += (operand.components == 4 ? 4 : 1) * (operand.type == Immediate64 ? 2 : 1);
        return i <= words.size();
    }

    const auto dimensions = (token >> 20) & 3;
    for (std::uint32_t d = 0; d < dimensions; ++d)
    {
        const auto representation = (token >> (22 + 3 * d)) & 7;
        std::uint32_t value{};
        Operand relative;
        if (representation == 0 || representation == 3)
        {
            if (i >= words.size()) return false;
            value = words[i++];
        }
        else if (representation == 1 || representation == 4) i += 2;
        if (representation >= 2 && !readOperand(words, i, relative)) return false;
        if (representation > 4) return false;
        if (d == 0) operand.index = value;
    }
    return i <= words.size();
}

std::optional<std::vector<Instruction>> decode(std::span<const std::uint32_t> program)
{
    std::vector<Instruction> result;
    for (std::size_t i = 2; i < program.size();)
    {
        const auto length = upscaler::instructionLength(program.subspan(i));
        if (!length) return std::nullopt;

        const auto opcode = program[i] & 0x7ff;
        if (!declaration(opcode) && opcode != CustomData)
        {
            Instruction instruction{opcode, {}};
            const auto words = program.subspan(0, i + length);
            std::size_t j = i + 1;
            for (auto extended = program[i]; extended & 0x80000000u && j < words.size();) extended = words[j++];
            while (j < words.size())
            {
                Operand operand;
                if (!readOperand(words, j, operand)) return std::nullopt;
                instruction.operands.push_back(operand);
            }
            result.push_back(std::move(instruction));
        }
        i += length;
    }
    return result;
}

// Components per register, keyed by operand type and register index.
using Components = std::map<std::uint64_t, std::uint8_t>;

std::uint64_t key(std::uint32_t type, std::uint32_t index) { return std::uint64_t{type} << 32 | index; }

// Components of `source` that the given destination components read.
std::uint8_t read(const Operand& source, std::uint8_t written, bool whole)
{
    if (source.components == 1) return 1;

    std::uint8_t components{};
    for (int c = 0; c < 4; ++c)
        if (whole || (written & (1 << c))) components |= static_cast<std::uint8_t>(1u << source.swizzle[c]);
    return components;
}

// Walks back from instruction `from`, following the temp components in `needed` to where they are
// written. Sampled values are followed into `samples` (instruction indices) instead of their
// coordinates when `samples` is given; otherwise coordinates are followed too. Returns the input
// components reached.
Components slice(const std::vector<Instruction>& program, std::size_t from, Components needed, std::vector<std::size_t>* samples)
{
    Components inputs;
    for (std::size_t i = from; i-- > 0;)
    {
        const auto& instruction = program[i];
        const auto count = destinations(instruction.opcode);
        std::uint8_t written{};
        for (std::size_t d = 0; d < count && d < instruction.operands.size(); ++d)
        {
            const auto& destination = instruction.operands[d];
            if (destination.type != TempOperand) continue;
            auto found = needed.find(key(TempOperand, destination.index));
            if (found == needed.end() || !(found->second & destination.mask)) continue;
            written |= found->second & destination.mask;
            found->second &= static_cast<std::uint8_t>(~destination.mask);
        }
        if (!written) continue;

        if (samples && sampling(instruction.opcode))
        {
            samples->push_back(i);
            continue;
        }
        const bool whole = wholeVector(instruction.opcode);
        for (std::size_t s = count; s < instruction.operands.size(); ++s)
        {
            const auto& source = instruction.operands[s];
            if (source.type == TempOperand) needed[key(TempOperand, source.index)] |= read(source, written, whole);
            else if (source.type == InputOperand) inputs[key(InputOperand, source.index)] |= read(source, written, whole);
        }
    }
    return inputs;
}

std::span<const std::uint8_t> chunk(const void* code, std::size_t size, std::initializer_list<const char*> tags)
{
    const auto* bytes = static_cast<const std::uint8_t*>(code);
    if (!code || size < 32 || std::memcmp(bytes, "DXBC", 4)) return {};

    std::uint32_t count{};
    std::memcpy(&count, bytes + 28, 4);
    for (std::uint32_t c = 0; c < count && 32 + c * 4 + 4 <= size; ++c)
    {
        std::uint32_t offset{}, length{};
        std::memcpy(&offset, bytes + 32 + c * 4, 4);
        if (offset + 8 > size) return {};
        std::memcpy(&length, bytes + offset + 4, 4);
        if (offset + 8 + std::uint64_t{length} > size) return {};
        for (const auto* tag : tags)
            if (!std::memcmp(bytes + offset, tag, 4)) return {bytes + offset + 8, length};
    }
    return {};
}

std::string typeName(const SignatureElement& element, int width)
{
    const char* base = element.componentType == 1 ? "uint" : element.componentType == 2 ? "int" : "float";
    return width == 1 ? base : std::string(base) + std::to_string(width);
}

bool sameElement(const SignatureElement& a, const SignatureElement& b)
{
    return a.semantic == b.semantic && a.semanticIndex == b.semanticIndex;
}

// The vertex shader for the bake: one triangle covering the viewport. Outputs must sit at the
// registers the pixel shader reads them from. The compiler packs consecutive small outputs into one
// register, which Bend's shaders mostly do not, so an input alone in its register is declared four
// wide to keep that register to itself; inputs sharing a register keep their own widths.
std::string bakeVertexShader(const std::vector<SignatureElement>& inputs, const AlphaTest& test)
{
    auto elements = inputs;
    std::erase_if(elements, [](const SignatureElement& element) { return element.systemValue > 3 && element.systemValue != PositionValue; });
    std::sort(elements.begin(), elements.end(), [](const SignatureElement& a, const SignatureElement& b)
    {
        return a.reg != b.reg ? a.reg < b.reg : std::countr_zero(static_cast<unsigned>(a.mask)) < std::countr_zero(static_cast<unsigned>(b.mask));
    });
    const bool position = std::any_of(elements.begin(), elements.end(), [](const SignatureElement& e) { return e.systemValue == PositionValue; });
    std::string declarations, assignments;
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        const auto& element = elements[i];
        const auto alone = std::count_if(elements.begin(), elements.end(), [&](const SignatureElement& e) { return e.reg == element.reg; }) == 1;
        const int width = alone ? 4 : std::popcount(static_cast<unsigned>(element.mask));
        const auto name = "e" + std::to_string(i);
        declarations += "    " + typeName(element, width) + " " + name + " : " + element.semantic + std::to_string(element.semanticIndex) + ";\n";
        if (element.systemValue == PositionValue) assignments += "    output." + name + " = position;\n";
        else assignments += "    output." + name + " = 1;\n";
        // Four wide, the element's components start where its mask does.
        const auto offset = alone ? static_cast<std::uint32_t>(std::countr_zero(static_cast<unsigned>(element.mask))) : 0u;
        if (sameElement(element, test.coordinates))
        {
            const char* swizzles[]{"xy", "yz", "zw"};
            assignments += "    output." + name + (width > 2 ? std::string(".") + swizzles[std::min(test.firstComponent + offset, 2u)] : "") + " = uv;\n";
        }
        if (test.fade && sameElement(element, *test.fade))
            assignments += "    output." + name + (width > 1 ? std::string(".") + "xyzw"[std::min(test.fadeComponent + offset, 3u)] : "") + " = Fade;\n";
        for (std::size_t t = 0; t < test.toggles.size(); ++t)
            if (sameElement(element, test.toggles[t].first))
                assignments += "    output." + name + (width > 1 ? std::string(".") + "xyzw"[std::min(test.toggles[t].second + offset, 3u)] : "")
                    + " = Combination >> " + std::to_string(t) + " & 1;\n";
    }
    if (!position) declarations += "    float4 position : SV_Position;\n";
    return "cbuffer Bake : register(b0)\n{\n    float Fade;\n    float Depth;\n    uint Combination;\n};\n\n"
        "struct Output\n{\n" + declarations + "};\n\nOutput main(uint vertex : SV_VertexID)\n{\n"
        "    float2 uv = float2(vertex == 1 ? 2 : 0, vertex == 2 ? 2 : 0);\n"
        "    float4 position = float4(uv * float2(2, -2) + float2(-1, 1), Depth, 1);\n"
        "    Output output;\n" + assignments + (position ? "" : "    output.position = position;\n") + "    return output;\n}\n";
}

// The input element holding a register component, if any.
const SignatureElement* elementAt(const std::vector<SignatureElement>& inputs, std::uint32_t reg, std::uint32_t component)
{
    const auto element = std::find_if(inputs.begin(), inputs.end(), [&](const SignatureElement& e)
    {
        return e.reg == reg && (e.mask & (1u << component));
    });
    return element == inputs.end() ? nullptr : &*element;
}
}

std::vector<SignatureElement> readSignature(const void* code, std::size_t size, bool inputs)
{
    std::vector<SignatureElement> result;
    const bool extended = !chunk(code, size, {inputs ? "ISG1" : "OSG1"}).empty();
    const auto data = chunk(code, size, {inputs ? (extended ? "ISG1" : "ISGN") : (extended ? "OSG1" : "OSGN")});
    if (data.size() < 8) return result;

    std::uint32_t count{};
    std::memcpy(&count, data.data(), 4);
    const std::size_t stride = extended ? 32 : 24, start = extended ? 4 : 0;
    for (std::uint32_t e = 0; e < count && 8 + (e + 1) * stride <= data.size(); ++e)
    {
        const auto* entry = data.data() + 8 + e * stride + start;
        std::uint32_t fields[5]{};
        std::memcpy(fields, entry, sizeof(fields));
        SignatureElement element;
        if (fields[0] < data.size())
            element.semantic.assign(reinterpret_cast<const char*>(data.data() + fields[0]),
                strnlen(reinterpret_cast<const char*>(data.data() + fields[0]), data.size() - fields[0]));
        element.semanticIndex = fields[1];
        element.systemValue = fields[2];
        element.componentType = fields[3];
        element.reg = fields[4];
        element.mask = entry[20];
        result.push_back(std::move(element));
    }
    return result;
}

bool discardsByTexture(const void* code, std::size_t size)
{
    auto bytecode = upscaler::readPixelBytecode(code, size);
    const auto program = bytecode.words.empty() ? std::nullopt : decode(bytecode.program());
    if (!program) return true;

    const auto discard = std::find_if(program->begin(), program->end(), [](const Instruction& i) { return i.opcode == Discard; });
    if (discard == program->end() || discard->operands.empty() || discard->operands[0].type != TempOperand) return false;

    std::vector<std::size_t> samples;
    const auto& condition = discard->operands[0];
    slice(*program, static_cast<std::size_t>(discard - program->begin()), {{key(TempOperand, condition.index), read(condition, 1, false)}}, &samples);
    return !samples.empty();
}

std::optional<AlphaTest> analyseAlphaTest(const void* code, std::size_t size)
{
    auto bytecode = upscaler::readPixelBytecode(code, size);
    if (bytecode.words.empty()) return std::nullopt;
    const auto program = decode(bytecode.program());
    if (!program) return std::nullopt;

    // The discard's condition, back to the texture samples and the inputs it depends on.
    const auto discard = std::find_if(program->begin(), program->end(), [](const Instruction& i) { return i.opcode == Discard; });
    if (discard == program->end() || discard->operands.empty() || discard->operands[0].type != TempOperand) return std::nullopt;
    std::vector<std::size_t> samples;
    const auto& condition = discard->operands[0];
    const auto values = slice(*program, static_cast<std::size_t>(discard - program->begin()),
        {{key(TempOperand, condition.index), read(condition, 1, false)}}, &samples);

    // Their coordinates, back to the pixel shader inputs they come from. Inputs sampled with directly
    // are the texture coordinates; others can be tangents or positions mixed into them.
    Components coordinates, direct;
    for (const auto index : samples)
    {
        const auto& source = (*program)[index].operands[1];
        const auto used = read(source, 0xf, true);
        if (source.type == InputOperand)
        {
            coordinates[key(InputOperand, source.index)] |= used;
            direct[key(InputOperand, source.index)] |= used;
        }
        else if (source.type == TempOperand)
            for (const auto& [input, components] : slice(*program, index, {{key(TempOperand, source.index), used}}, nullptr))
                coordinates[input] |= components;
    }

    // Prefer direct coordinates, then UE4's texture coordinate interpolants (TEXCOORD0 to 7; higher
    // indices carry tangents and other data), never a system value such as the screen position.
    const auto inputs = readSignature(code, size, true);
    std::optional<AlphaTest> best;
    int bestRank = -1;
    for (const auto& [input, components] : coordinates)
    {
        const auto reg = static_cast<std::uint32_t>(input & 0xffffffff);
        const auto first = static_cast<std::uint32_t>(std::countr_zero(static_cast<unsigned>(components)));
        const auto* element = elementAt(inputs, reg, first);
        if (!element || element->systemValue != 0) continue;

        const int rank = (direct.contains(input) ? 2 : 0) + (element->semantic == "TEXCOORD" && element->semanticIndex < 8 ? 1 : 0);
        if (rank <= bestRank) continue;
        bestRank = rank;
        best = AlphaTest{*element, first - static_cast<std::uint32_t>(std::countr_zero(static_cast<unsigned>(element->mask))), {}};
    }
    if (!best) return best;

    // Bend's prepass tests mostly scale the opacity by one interpolated value besides the texture: a
    // fade or a vertex colour channel. Screen positions (dithering) and the coordinates are not it.
    // With more than one, they are the bake's toggles.
    std::vector<std::pair<SignatureElement, std::uint32_t>> others;
    for (const auto& [input, components] : values)
        for (std::uint32_t c = 0; c < 4; ++c)
        {
            const auto* element = (components & (1u << c)) ? elementAt(inputs, static_cast<std::uint32_t>(input & 0xffffffff), c) : nullptr;
            if (!element || element->systemValue != 0) continue;

            const auto component = c - static_cast<std::uint32_t>(std::countr_zero(static_cast<unsigned>(element->mask)));
            if (sameElement(*element, best->coordinates) && component >= best->firstComponent && component < best->firstComponent + 2) continue;

            others.push_back({*element, component});
        }
    if (others.size() == 1)
    {
        best->fade = others.front().first;
        best->fadeComponent = others.front().second;
    }
    else
    {
        others.resize(std::min(others.size(), MaxToggles));
        best->toggles = std::move(others);
    }

    best->bakeVertexShader = bakeVertexShader(inputs, *best);
    // dcl_resource: the opcode token, the resource operand token, then its register.
    const auto words = bytecode.program();
    for (std::size_t i = 2; i + 2 < words.size(); i += upscaler::instructionLength(words.subspan(i)))
        if ((words[i] & 0x7ff) == ResourceDeclaration && words[i + 2] < 32) best->textures |= 1u << words[i + 2];
    return best;
}
}
