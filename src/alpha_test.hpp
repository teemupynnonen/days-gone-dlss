#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Alpha-tested materials, for the ray tracer. Bend alpha-tests in its depth-only passes with pixel
// shaders that discard below an opacity threshold; their opacity can combine many texture samples
// and arbitrary material maths. Rather than interpreting that, the ray tracer bakes each material's
// coverage by running the game's own pixel shader over its texture coordinates. This finds which
// pixel shader input holds those coordinates and makes the vertex shader that feeds the bake.
namespace days_gone_dlss::ray_tracing
{
struct SignatureElement
{
    std::string semantic;
    std::uint32_t semanticIndex{}, systemValue{}, componentType{}, reg{};
    std::uint8_t mask{};
};

inline constexpr std::size_t MaxToggles = 4;

// A value the coordinates are scaled or offset by: a component of a constant buffer register, or a
// literal.
struct TransformValue
{
    bool constant{};
    std::uint32_t buffer{}, reg{}, component{};
    float literal{};

    bool operator==(const TransformValue&) const = default;
};

// Materials tile a texture across a mesh in the pixel shader: the alpha test samples the coordinates
// times `scale` plus `offset`, per axis.
struct CoordinateTransform
{
    std::array<TransformValue, 2> scale, offset;

    bool operator==(const CoordinateTransform&) const = default;
};

struct AlphaTest
{
    // The input the opacity samples take their coordinates from, and its first used component
    // within that element (0 for .xy of a float4, 2 for .zw).
    SignatureElement coordinates;
    std::uint32_t firstComponent{};
    // How every sample the test reads scales the coordinates, if they do. The bake then covers one
    // texture tile rather than the mesh's coordinates from 0 to 1, where a finely tiled mask (a
    // chain-link fence) would fill every texel.
    std::optional<CoordinateTransform> transform;
    // A per-vertex value the test also reads, such as a fade or a vertex colour channel scaling the
    // opacity, and its component within the element. Only when the test reads exactly one.
    std::optional<SignatureElement> fade;
    std::uint32_t fadeComponent{};
    // The per-vertex values the test reads when there is not exactly one, such as the fade-in and
    // crossfade of Bend's LOD dither, with their components (at most MaxToggles). The bake tries
    // each at 0 and at 1 and keeps every texel some combination covers: one of them shows the mesh
    // fully faded in.
    std::vector<std::pair<SignatureElement, std::uint32_t>> toggles;
    // HLSL for a vertex shader whose outputs match the pixel shader's inputs: a triangle covering
    // the viewport, with `coordinates` running over [0, 1] (over one tile with a transform), `fade`
    // from the constant Fade, toggle t from bit t of the constant Combination and every other input 1.
    // The triangle lies at the constant Depth (all in b0). A transform's constants are the rows
    // Transform * 4 + k of the buffer in t0: the scale's x and y, then the offset's.
    std::string bakeVertexShader;
    // Texture slots the pixel shader declares (bit n for tn): what identifies its material.
    std::uint32_t textures{};
};

// Whether a pixel shader discards depending on a texture: an alpha test. Shaders that discard by a
// screen-space dither alone fade whole meshes in and out (Bend's LOD transitions) and are otherwise
// solid. Shaders that cannot be decoded count as alpha-tested.
bool discardsByTexture(const void* code, std::size_t size);
// The signature elements of a DXBC container's ISGN/ISG1 or OSGN/OSG1 chunk.
std::vector<SignatureElement> readSignature(const void* code, std::size_t size, bool inputs);
// Analyses a ps_5_0 pixel shader that discards; empty when it has no discard, or when its discard
// does not depend on a texture sampled with coordinates from a single input.
std::optional<AlphaTest> analyseAlphaTest(const void* code, std::size_t size);
}
