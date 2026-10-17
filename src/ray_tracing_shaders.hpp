#pragma once

// Direct3D 11 HLSL for the ray-tracing experiment, compiled at runtime with d3dcompiler_47. The
// tracing itself needs Shader Model 6.5 and is compiled at build time (ray_tracing_trace.hlsl).
namespace days_gone_dlss::ray_tracing::shaders
{
// Turns the streamed clip-space vertices into world positions in the texture shared with D3D12.
// Buffers cannot be shared between Direct3D 11 and 12, so the texture carries them, 4096 a row.
// Opaque vertices come from the G-buffer pass and go first; alpha-tested ones come with their
// texture coordinates and fade from the depth prepass and go at MaskedStart, their coordinates and
// fade, with their draw's coverage slot, to a texture of their own. Both ranges start on a triangle.
// Positions come from clip x, y and w: some draws, Bend's foliage among them, narrow the viewport's
// depth range and scale clip z to match, which would move their vertices along the view ray. Clip z
// still tells the sky, drawn at reversed-Z depth 0, at infinity, where the world position has no
// finite value. A NaN x on every vertex marks such a triangle inactive; any other NaN or infinity is
// undefined for DXR and in practice leaves the whole acceleration structure without hits. Shadow
// casters from the sun shadow maps follow the view's geometry in both ranges; their place in their
// map gives their position.
// The coordinates of a material that tiles its texture are scaled into the tile its coverage holds.
inline constexpr char Positions[] = R"(
cbuffer PositionParameters : register(b0)
{
    row_major float4x4 WorldFromClip;
    row_major float4x4 WorldFromClipXYW; // From clip x, y, w and 1
    uint OpaqueCount;
    uint MaskedCount;
    uint MaskedStart;
    uint CasterRunCount;
    float2 AtlasSize;
    uint TransformCount;
    uint MaterialDrawCount;
    uint4 CasterRuns[64];    // Runs of shadow casters: first opaque and alpha-tested vertex, shadow map
    float4 CasterDepths[64]; // Each run's viewport depth range
};
// A tiled draw's alpha-tested vertices, its four rows in TransformRows and, per value (scale x and y,
// offset x and y), whether its row holds it (bit k) and in which component (bits 8 + 2k), or else
// the literal.
struct TransformDraw
{
    uint First, End, Row, Sources;
    float4 Literals;
};
ByteAddressBuffer Streamed : register(t0);       // float4 clip position
ByteAddressBuffer MaskedStreamed : register(t1); // float4 clip position, float2 texture coordinates, float fade
ByteAddressBuffer LightSpace : register(t2);     // Rows 28 to 35 of the sun shadow resolve's constants
StructuredBuffer<TransformDraw> TransformDraws : register(t3); // By first vertex
ByteAddressBuffer TransformRows : register(t4);  // Copied from the draws' constants
StructuredBuffer<uint2> MaterialDraws : register(t5); // Per alpha-tested draw: first vertex, coverage slot
RWTexture2D<float4> Positions : register(u0);
RWTexture2D<float4> Coordinates : register(u1);  // Texture coordinates, fade and coverage slot

float TransformValue(TransformDraw draw, uint k)
{
    if (!(draw.Sources >> k & 1)) return draw.Literals[k];
    float4 row = asfloat(TransformRows.Load4((draw.Row * 4 + k) * 16));
    return row[draw.Sources >> (8 + 2 * k) & 3];
}

// An alpha-tested vertex's coordinates, scaled into its material's tile if it tiles.
float2 TiledCoordinates(uint local, float2 coordinates)
{
    uint low = 0, high = TransformCount;
    while (low < high)
    {
        uint middle = (low + high) / 2;
        if (TransformDraws[middle].First <= local) low = middle + 1;
        else high = middle;
    }
    if (!low || local >= TransformDraws[low - 1].End) return coordinates;

    TransformDraw draw = TransformDraws[low - 1];
    float2 scale = float2(TransformValue(draw, 0), TransformValue(draw, 1));
    float2 offset = float2(TransformValue(draw, 2), TransformValue(draw, 3));
    return coordinates * scale + offset;
}

// The coverage slot of an alpha-tested vertex's draw, -1 without one. Each vertex carries it, so the
// rays' alpha test, run for every candidate, needs no search of its own. It goes through the float
// texture as a float, exact for every slot: as bits, small slots would be denormals and no material a
// NaN, which a typed store need not keep.
float MaterialSlot(uint local)
{
    uint low = 0, high = MaterialDrawCount;
    while (low < high)
    {
        uint middle = (low + high) / 2;
        if (MaterialDraws[middle].x <= local) low = middle + 1;
        else high = middle;
    }
    return low ? float(MaterialDraws[low - 1].y) : -1;
}

float4 Clip(uint vertex, bool masked)
{
    return asfloat(masked ? MaskedStreamed.Load4(vertex * 28) : Streamed.Load4(vertex * 16));
}

// The homogeneous w of the world position: 0 at infinity.
float Finite(uint vertex, bool masked)
{
    return mul(Clip(vertex, masked), WorldFromClip).w;
}

float4 LightRow(uint row)
{
    return asfloat(LightSpace.Load4((row - 28) * 16));
}

// The run a caster vertex belongs to.
uint CasterRun(uint local, bool masked)
{
    uint found = 0;
    for (uint run = 0; run < CasterRunCount; ++run)
        if (local >= (masked ? CasterRuns[run].y : CasterRuns[run].x)) found = run;
    return found;
}

// The resolve takes the main view's translated world to light space with rows 28 to 31 (row
// vectors): the far map's u and v, and the depth all three maps share. The near cascades are centred
// on rows 34 and 35 (xy) and scaled by row 34.z and its square, the three maps side by side in the
// atlas, with row 35.z scaling u into it. A caster's place in its map (0 the far map, 1 and 2 the
// near cascades) and its depth after the viewport's depth range invert that.
float3 CasterWorld(float4 clip, uint run)
{
    uint map = CasterRuns[run].z;
    float3 device = clip.xyz / clip.w;
    device.z = lerp(CasterDepths[run].x, CasterDepths[run].y, device.z);
    float2 pixel = float2(map + device.x * 0.5 + 0.5, 0.5 - device.y * 0.5) * AtlasSize.y;
    float2 uv = float2(pixel.x / AtlasSize.x / LightRow(35).z, pixel.y / AtlasSize.y);
    if (map == 1) uv = (uv - float2(1.5, 0.5)) / LightRow(34).z + LightRow(34).xy;
    if (map == 2) uv = (uv - float2(2.5, 0.5)) / (LightRow(34).z * LightRow(34).z) + LightRow(35).xy;
    float3 light = float3(uv, device.z) - LightRow(31).xyz;
    // The inverse of the matrix with rows a, b and c has columns cross(b, c), cross(c, a) and
    // cross(a, b), over its determinant.
    float3 a = LightRow(28).xyz, b = LightRow(29).xyz, c = LightRow(30).xyz;
    return float3(dot(light, cross(b, c)), dot(light, cross(c, a)), dot(light, cross(a, b))) / dot(a, cross(b, c));
}

[numthreads(1024, 1, 1)]
void main(uint index : SV_DispatchThreadID)
{
    if (index >= OpaqueCount + MaskedCount) return;
    bool masked = index >= OpaqueCount;
    uint local = masked ? index - OpaqueCount : index, first = local - local % 3;
    uint vertex = masked ? MaskedStart + local : local;
    float4 position;
    if (CasterRunCount && local >= (masked ? CasterRuns[0].y : CasterRuns[0].x))
        position = float4(CasterWorld(Clip(local, masked), CasterRun(local, masked)), 1);
    else
    {
        float finite = min(Finite(first, masked), min(Finite(first + 1, masked), Finite(first + 2, masked)));
        float3 world = mul(float4(Clip(local, masked).xyw, 1), WorldFromClipXYW).xyz;
        position = finite > 1e-6 ? float4(world, 1) : float4(asfloat(0x7fc00000), 0, 0, 1);
    }
    Positions[uint2(vertex % 4096, vertex / 4096)] = position;
    if (masked)
    {
        float3 coordinates = asfloat(MaskedStreamed.Load3(local * 28 + 16));
        Coordinates[uint2(local % 4096, local / 4096)] = float4(TiledCoordinates(local, coordinates.xy), coordinates.z, MaterialSlot(local));
    }
}
)";

// Reduces a material's baked coverage (each sample's lowest passing fade level, 1 where none passes)
// into its slot of the coverage atlas shared with D3D12. A slot texel passes once CoveredSamples of
// its samples do: thin needles stay, and a stray sample does not fill the gaps between them.
inline constexpr char BakeCopy[] = R"(
cbuffer BakeParameters : register(b0)
{
    uint2 Slot; // Texel origin of the slot
    uint2 Padding;
};
Texture2D<float> Baked : register(t0);
RWTexture2D<unorm float> Coverage : register(u0);

static const uint Samples = 4;        // Per slot texel side, as BakeSamples
static const uint CoveredSamples = 4; // Of Samples * Samples

[numthreads(8, 8, 1)]
void main(uint2 texel : SV_DispatchThreadID)
{
    // The texel's level is the CoveredSamples-th lowest of its samples'; lowest stays sorted.
    float lowest[CoveredSamples];
    for (uint i = 0; i < CoveredSamples; ++i) lowest[i] = 1;
    for (uint y = 0; y < Samples; ++y)
        for (uint x = 0; x < Samples; ++x)
        {
            float level = Baked[texel * Samples + uint2(x, y)];
            for (uint j = 0; j < CoveredSamples; ++j)
                if (level < lowest[j])
                {
                    float larger = lowest[j];
                    lowest[j] = level;
                    level = larger;
                }
        }
    Coverage[Slot + texel] = lowest[CoveredSamples - 1];
}
)";

// Copies the game's depth over the view into the texture shared with D3D12, where sun rays start.
inline constexpr char Depth[] = R"(
cbuffer RegionParameters : register(b0)
{
    int2 RenderOrigin;
    uint2 Size;
};
Texture2D<float> GameDepth : register(t0);
RWTexture2D<float> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    Output[pixel] = GameDepth[int2(pixel) + RenderOrigin];
}
)";

inline constexpr char FullScreen[] = R"(
float4 main(uint vertex : SV_VertexID) : SV_Position
{
    return float4(float2(vertex == 1 ? 3 : -1, vertex == 2 ? -3 : 1), 0, 1);
}
)";

// Adds the traced sun shadows to the game's resolved ones before lighting reads them. The target's
// red channel holds the square root of the sun's visibility (lighting squares it and skips the sun
// at 0); taking the lower visibility keeps the shadows the game sees and the trace misses (foliage,
// off-screen casters). Surfaces facing away from the sun are left to the lighting's N.L. Writes red only.
// With soft shadows the visibility comes filtered.
inline constexpr char Inject[] = R"(
cbuffer RegionParameters : register(b0)
{
    int2 RenderOrigin;
    uint2 Size;
};
Texture2D<float4> Sun : register(t0);        // r: traced visibility, g: N.L
Texture2D<float2> GameShadow : register(t1); // The game's target before this pass
Texture2D<float4> Visibility : register(t2); // r: the visibility to use, Sun's own or filtered

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 local = int2(position.xy) - RenderOrigin;
    if (any(local < 0) || any(local >= int2(Size))) discard;
    if (Sun.Load(int3(local, 0)).g <= 0) discard;
    float game = GameShadow.Load(int3(position.xy, 0)).x;
    return float4(sqrt(min(game * game, saturate(Visibility.Load(int3(local, 0)).r))), 0, 0, 0);
}
)";

// Combines the traced ambient occlusion with the game's own for its blur: of each corner's 6-bit
// visibility the lower of the two, with the depth Bend keeps in the top bytes. The game's gives the
// near contact the trees' copies leave out on foliage (they occlude it from 30 cm); the trace adds
// what the screen does not hold. The trace leaves the corners it skips fully visible, so
// the game's stay there. Texels outside the view keep the game's.
inline constexpr char AmbientPack[] = R"(
cbuffer RegionParameters : register(b0)
{
    int2 RenderOrigin;
    uint2 Size;
};
Texture2D<uint4> Traced : register(t0);   // Over the view, in x and y
Texture2D<uint2> Game : register(t1);     // Over the game's target
RWTexture2D<uint2> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 texel : SV_DispatchThreadID)
{
    uint width, height;
    Output.GetDimensions(width, height);
    if (any(texel >= uint2(width, height))) return;
    uint2 game = Game[texel];
    int2 local = int2(texel) - RenderOrigin;
    if (any(local < 0) || any(local >= int2(Size)))
    {
        Output[texel] = game;
        return;
    }

    uint2 traced = Traced[local].xy, combined = game & 0xff000000;
    for (uint shift = 0; shift < 24; shift += 6)
        combined |= min(traced >> shift & 63, game >> shift & 63) << shift;
    Output[texel] = combined;
}
)";

// Copies the game's diffuse colours over the view into the texture shared with D3D12 for the bounced
// light. The stencil's low 4 bits are the lighting model, each lit by its own full-screen draw:
// models 1 to 3 keep the square root of the base colour in 8 bits a channel of the first target,
// model 1 also its metalness, which takes from the diffuse colour. The others (hair, emissive) pack
// theirs differently and are left unknown.
inline constexpr char BounceSurfaces[] = R"(
cbuffer RegionParameters : register(b0)
{
    int2 RenderOrigin;
    uint2 Size;
};
Texture2D<uint2> GBuffer : register(t0);   // Bend's first G-buffer target
Texture2D<uint2> Stencil : register(t1);   // y: the stencil
RWTexture2D<float4> Albedo : register(u0); // rgb: diffuse colour, a: 1 where known

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    int3 texel = int3(int2(pixel) + RenderOrigin, 0);
    uint2 packed = GBuffer.Load(texel);
    uint model = Stencil.Load(texel).y & 15;
    float3 color = float3(packed.x & 255, packed.x >> 8 & 255, packed.x >> 16 & 255) / 255;
    float diffuse = model == 1 ? 1 - (packed.y & 255) / 255.0 : 1;
    Albedo[pixel] = model >= 1 && model <= 3 ? float4(color * color * diffuse, 1) : 0;
}
)";

// Copies the game's G-buffer normal over the view into the texture shared with D3D12, the normal in
// bits 0 to 19 and the lighting model, the stencil's low 4 bits, in bits 28 to 31: rays keep the
// trees' copies off foliage's own leaves (ray_tracing_scene.hlsli). 0 stays 0, unknown.
inline constexpr char GameNormals[] = R"(
cbuffer RegionParameters : register(b0)
{
    int2 RenderOrigin;
    uint2 Size;
};
Texture2D<uint> GBuffer : register(t0);  // Bend's second G-buffer target
Texture2D<uint2> Stencil : register(t1); // y: the stencil
RWTexture2D<uint> Normals : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    int3 texel = int3(int2(pixel) + RenderOrigin, 0);
    uint normal = GBuffer.Load(texel) & 0xfffff;
    Normals[pixel] = normal ? normal | (Stencil.Load(texel).y & 15) << 28 : 0;
}
)";

// Accumulates a traced signal over frames (the bounced light, the lights' visibility): each pixel
// finds where its surface was last frame and blends this frame's rays into what it had there, up to
// MaxFrames. History from a depth that does not match belongs to another surface, which the pixel
// did not see then. With Clamp, a pixel far brighter than all its neighbours (a bounce ray that found
// the brightest spot near a light) is brought down to twice the brightest of them: foliage keeps
// little history, and such a pixel would show as a firefly.
inline constexpr char BounceAccumulate[] = R"(
cbuffer BounceParameters : register(b0)
{
    row_major float4x4 WorldFromClip;
    row_major float4x4 PreviousClipFromWorld; // This frame's translated world to last frame's clip
    uint2 Size;
    uint Step;      // Filter: texels between taps
    uint History;   // Last frame's accumulation covers this view
    uint Final;     // Filter: the last pass turns the sun's bounce into the scene colour's units
    uint MaxFrames; // Accumulation: frames a pixel keeps
    uint Clamp;     // Accumulation: bring down lone bright pixels
    uint Padding;
};
Texture2D<float4> Traced : register(t0);       // rgb: this frame's signal, a: 1 on the game's surfaces
Texture2D<float4> Previous : register(t1);     // rgb: last frame's accumulation, a: its frames
Texture2D<float> Depth : register(t2);         // The game's device depth over the view
Texture2D<float> PreviousDepth : register(t3); // Last frame's
RWTexture2D<float4> Output : register(u0);

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    float4 traced = Traced[pixel];
    if (traced.a == 0)
    {
        Output[pixel] = 0;
        return;
    }
    if (Clamp)
    {
        float brightest = 0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 texel = clamp(int2(pixel) + int2(x, y), 0, int2(Size) - 1);
                if ((x || y) && Traced[texel].a > 0) brightest = max(brightest, Luminance(Traced[texel].rgb));
            }
        float own = Luminance(traced.rgb), limit = 2 * brightest;
        if (own > limit) traced.rgb *= limit / own;
    }

    float4 history = 0;
    float2 screen = (pixel + 0.5) / Size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(screen, Depth[pixel], 1), WorldFromClip);
    float4 clip = mul(float4(world.xyz / world.w, 1), PreviousClipFromWorld);
    if (History && clip.w > 0)
    {
        float2 position = (clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * Size - 0.5;
        int2 corner = int2(floor(position));
        float2 fraction = position - corner;
        float4 sum = 0;
        float weights = 0;
        for (uint i = 0; i < 4; ++i)
        {
            int2 texel = corner + int2(i & 1, i >> 1);
            if (any(texel < 0) || any(texel >= int2(Size))) continue;

            float4 previous = Previous[texel];
            // Reversed-Z device depth is proportional to 1 / view depth.
            if (previous.a == 0 || abs(PreviousDepth[texel] * clip.w / clip.z - 1) > 0.03) continue;

            float weight = (i & 1 ? fraction.x : 1 - fraction.x) * (i >> 1 ? fraction.y : 1 - fraction.y);
            sum += previous * weight;
            weights += weight;
        }
        if (weights > 0.01) history = sum / weights;
    }
    float frames = min(history.a + 1, float(MaxFrames));
    Output[pixel] = float4(lerp(history.rgb, traced.rgb, 1 / frames), frames);
}
)";

// One pass of an edge-avoiding a-trous filter over an accumulated signal: a 5 by 5 B3-spline kernel
// with Step texels between taps, weighted down by distance from the pixel's plane and by normals
// apart. For the sun's bounce, the last pass turns the light into the scene colour's units as Bend's
// lighting does with the sun: its colour, which grows with height above the world's origin, the
// exposure, and 1 / pi for a diffuse surface.
inline constexpr char BounceFilter[] = R"(
cbuffer BounceParameters : register(b0)
{
    row_major float4x4 WorldFromClip;
    row_major float4x4 PreviousClipFromWorld;
    uint2 Size;
    uint Step;
    uint History;
    uint Final;
    uint MaxFrames;
    uint Clamp;
    uint Padding;
};
Texture2D<float4> Input : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<uint> Normals : register(t2);
// Rows of the lighting's constants: the sun's colour at height 0, the exposure (y), the colour the sun
// tends to with height (doubled) and the camera's height (z).
ByteAddressBuffer Lighting : register(t3);
RWTexture2D<float4> Output : register(u0);

static const float Kernel[3] = { 3.0 / 8, 1.0 / 4, 1.0 / 16 };

float3 Unproject(int2 texel, float depth)
{
    float2 screen = (texel + 0.5) / Size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(screen, depth, 1), WorldFromClip);
    return world.xyz / world.w;
}

float3 DecodeNormal(uint packed)
{
    float2 encoded = float2(packed & 1023, packed >> 10 & 1023) * (2.0 / 1023) - 1;
    float3 normal = float3(encoded, 1 - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0) normal.xy = (1 - abs(normal.yx)) * (normal.xy >= 0 ? 1 : -1);
    return normalize(normal);
}

float4 LightingRow(uint row)
{
    return asfloat(Lighting.Load4(row * 16));
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    if (Input[pixel].a == 0)
    {
        Output[pixel] = 0;
        return;
    }

    float3 position = Unproject(pixel, Depth[pixel]), normal = DecodeNormal(Normals[pixel]);
    float tolerance = 10 + 0.01 * length(position - Unproject(pixel, 1));
    float3 sum = 0;
    float weights = 0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
        {
            int2 texel = int2(pixel) + int2(x, y) * int(Step);
            if (any(texel < 0) || any(texel >= int2(Size))) continue;

            float4 value = Input[texel];
            if (value.a == 0) continue;

            float apart = abs(dot(normal, Unproject(texel, Depth[texel]) - position));
            float weight = Kernel[abs(x)] * Kernel[abs(y)] * exp(-apart / tolerance)
                * pow(saturate(dot(normal, DecodeNormal(Normals[texel]))), 8);
            sum += value.rgb * weight;
            weights += weight;
        }
    float3 light = sum / weights;
    if (Final)
    {
        float height = max((position.z + LightingRow(3).z) * 1.6e-5, 0);
        float3 sun = LightingRow(0).rgb + height * (2 * LightingRow(2).rgb - LightingRow(0).rgb);
        light *= sun * LightingRow(1).y / 3.14159265;
    }
    Output[pixel] = float4(light, 1);
}
)";

// Adds the bounced light to the scene colour once every lighting model has drawn: the filtered light
// times the surface's diffuse colour. Pixels of unknown colour are left to the game. Additive.
inline constexpr char BounceAdd[] = R"(
cbuffer RegionParameters : register(b0)
{
    int2 RenderOrigin;
    uint2 Size;
};
Texture2D<float4> Light : register(t0);  // Over the view, in the scene colour's units
Texture2D<float4> Albedo : register(t1); // rgb: diffuse colour, a: 1 where known

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 local = int2(position.xy) - RenderOrigin;
    if (any(local < 0) || any(local >= int2(Size))) discard;
    float4 albedo = Albedo.Load(int3(local, 0));
    if (albedo.a == 0) discard;
    return float4(albedo.rgb * Light.Load(int3(local, 0)).rgb, 0);
}
)";

// Puts the local lights' constants, copied from the game's buffers, and their scissor rectangles in
// the texture shared with D3D12: a row per light, the rectangle last. A last row holds the lighting
// constants' row 43, whose y is the exposure.
inline constexpr char LightTable[] = R"(
cbuffer TableParameters : register(b0)
{
    int4 Rects[16];
};
ByteAddressBuffer Constants : register(t0); // Four rows a light, then the lighting row
RWTexture2D<float4> Table : register(u0);

[numthreads(5, 17, 1)]
void main(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x < 4) Table[cell] = asfloat(Constants.Load4((cell.y * 4 + cell.x) * 16));
    else Table[cell] = cell.y < 16 ? asfloat(Rects[cell.y]) : 0;
}
)";

// Adds the local lights to the scene colour: each pixel's lights as the game drew them, unshadowed,
// times the share of them that reaches it (as traced, or filtered for soft shadows), and the light
// they bounced toward it times its diffuse colour. Additive.
inline constexpr char LightsAdd[] = R"(
cbuffer RegionParameters : register(b0)
{
    int2 RenderOrigin;
    uint2 Size;
};
Texture2D<float4> Unshadowed : register(t0); // Over the view
Texture2D<float4> Visibility : register(t1); // rgb: the share of the lights that reaches the pixel
Texture2D<float4> Bounce : register(t2);     // The light bounced toward the pixel, when traced
Texture2D<float4> Albedo : register(t3);     // rgb: diffuse colour, a: 1 where known

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 local = int2(position.xy) - RenderOrigin;
    if (any(local < 0) || any(local >= int2(Size))) discard;
    int3 texel = int3(local, 0);
    float3 light = Unshadowed.Load(texel).rgb * saturate(Visibility.Load(texel).rgb);
    float4 albedo = Albedo.Load(texel);
    if (albedo.a > 0) light += albedo.rgb * Bounce.Load(texel).rgb;
    return float4(light, 0);
}
)";

// Shows a debug view over the back buffer. Geometry shades the traced triangles; sun light lights
// them with the traced sun alone. The depth comparison colours each pixel by how the traced hit
// relates to the game's depth: green where they agree, red where the trace hits something nearer
// (geometry the game discards, such as alpha-tested foliage), blue where it hits farther or misses
// (geometry not captured). The shadow comparison puts the game's sun visibility in red and the
// traced one in green: yellow where both are lit, red where only the game is, green where only the
// trace is. Surfaces facing away from the sun are grey there: the game leaves those to N.L. Ambient
// occlusion, the traced and the game's combined, is shown as the lighting reads it after the game's
// blur: for each corner pair the corner on the normal's side, weighted by its squared cosine. The
// comparison shows the game's alone on the left and the combined on the right. Global illumination
// shows the bounced light as it is added to the scene, mapped from its range with x / (1 + x) and
// the square root the game's colour uses.
inline constexpr char Composite[] = R"(
static const uint Geometry = 0, DepthComparison = 1, SunLight = 2, ShadowComparison = 3, AmbientOcclusion = 4, AmbientComparison = 5,
    OffscreenShadows = 6, GlobalIllumination = 7;
static const float3 Corners[4] = { float3(-1, -1, 1), float3(1, -1, 1), float3(-1, 1, 1), float3(1, 1, 1) };

cbuffer CompositeParameters : register(b0)
{
    int2 OutputOrigin;
    int2 RenderOrigin;
    float2 RenderScale; // Render pixels per output pixel
    uint Mode;
    uint Padding;
};
Texture2D<float4> Traced : register(t0);     // rgb: shaded geometry, a: device depth of the hit
Texture2D<float> GameDepth : register(t1);
Texture2D<float2> GameShadow : register(t2); // x: sqrt of the game's sun visibility
Texture2D<float4> Sun : register(t3);        // r: traced sun visibility, g: N.L, a: through what survives off screen
Texture2D<uint2> Ambient : register(t4);     // The traced and the game's ambient occlusion combined, blurred
Texture2D<uint2> GameAmbient : register(t5); // The game's own
Texture2D<uint> Normals : register(t6);      // The game's G-buffer normal over the view, 0 for sky
Texture2D<float4> Bounce : register(t7);     // The bounced light over the view, in the scene colour's units
Texture2D<float4> Albedo : register(t8);     // rgb: the game's diffuse colour over the view, a: 1 where known

float3 DecodeNormal(uint packed)
{
    float2 encoded = float2(packed & 1023, packed >> 10 & 1023) * (2.0 / 1023) - 1;
    float3 normal = float3(encoded, 1 - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0) normal.xy = (1 - abs(normal.yx)) * (normal.xy >= 0 ? 1 : -1);
    return normalize(normal);
}

float AmbientVisibility(uint2 packed, float3 normal)
{
    float sum = 0, weights = 0;
    for (uint pair = 0; pair < 4; ++pair)
    {
        float facing = dot(normal, Corners[pair]);
        uint slot = pair * 2 + (facing > 0 ? 0 : 1);
        uint bits = slot < 4 ? packed.x >> (slot * 6) : packed.y >> (slot * 6 - 24);
        sum += (bits & 63) / 63.0 * facing * facing;
        weights += facing * facing;
    }
    return sum / weights;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 local = int2((position.xy - OutputOrigin) * RenderScale);
    uint width, height;
    Traced.GetDimensions(width, height);
    if (any(local < 0) || any(local >= int2(width, height))) discard;
    float4 traced = Traced.Load(int3(local, 0));
    float4 sun = Sun.Load(int3(local, 0));
    if (Mode == Geometry) return float4(traced.rgb, 1);
    if (Mode == SunLight) return float4((traced.a > 0 ? 0.15 + 0.85 * sun.r * saturate(sun.g) : 0).xxx, 1);
    if (Mode == ShadowComparison)
    {
        if (traced.a <= 0) return float4(0.02, 0.02, 0.05, 1);
        if (sun.g <= 0) return float4(0.25, 0.25, 0.25, 1);
        float game = GameShadow.Load(int3(local + RenderOrigin, 0)).x;
        return float4(game * game, sun.r, 0, 1);
    }
    // Red where the traced shadow would go once its casters leave the view (only the view's capture
    // holds them), blue where only the geometry kept off screen shadows. Dark grey: shadowed by both;
    // light grey: lit by both; navy: facing away from the sun, or sky.
    if (Mode == OffscreenShadows)
    {
        if (traced.a <= 0 || sun.g <= 0) return float4(0.05, 0.05, 0.15, 1);
        if (sun.r < 0.5 && sun.a >= 0.5) return float4(1, 0, 0, 1);
        if (sun.r >= 0.5 && sun.a < 0.5) return float4(0, 0.3, 1, 1);
        return float4((sun.r < 0.5 ? 0.2 : 0.8).xxx, 1);
    }
    if (Mode == AmbientOcclusion || Mode == AmbientComparison)
    {
        uint normal = Normals.Load(int3(local, 0));
        if (!normal) return float4(0.02, 0.02, 0.05, 1);
        if (Mode == AmbientComparison && abs(local.x * 2 - int(width)) <= 1) return float4(1, 0.8, 0.2, 1);
        bool game = Mode == AmbientComparison && local.x * 2 < int(width);
        uint2 packed = game ? GameAmbient.Load(int3(local + RenderOrigin, 0)) : Ambient.Load(int3(local + RenderOrigin, 0));
        return float4(AmbientVisibility(packed, DecodeNormal(normal)).xxx, 1);
    }
    if (Mode == GlobalIllumination)
    {
        float4 albedo = Albedo.Load(int3(local, 0));
        if (albedo.a == 0) return float4(0.02, 0.02, 0.05, 1);
        float3 light = albedo.rgb * Bounce.Load(int3(local, 0)).rgb;
        return float4(sqrt(light / (1 + light)), 1);
    }

    float game = GameDepth.Load(int3(local + RenderOrigin, 0));
    float shade = dot(traced.rgb, 1.0 / 3);
    if (traced.a <= 0 && game <= 0) return float4(0.02, 0.02, 0.05, 1);
    // Reversed-Z device depth is proportional to 1 / view depth.
    float ratio = traced.a / max(game, 1e-12);
    float3 tint = abs(ratio - 1) < 0.01 ? float3(0.1, 0.9, 0.2) : ratio > 1 ? float3(1, 0.15, 0.1) : float3(0.15, 0.3, 1);
    return float4(tint * (0.35 + 0.65 * shade), 1);
}
)";
}
