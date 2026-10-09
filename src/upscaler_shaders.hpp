#pragma once

// HLSL for the DLSS integration, compiled at runtime with d3dcompiler_47.
//
// Bend's TAA (BendTemporalAA.usf, TemporalAACS_T1X) works on tonemapped colour encoded with a
// square root, then its tail squares it, applies vignette, film grain and the Slate UI and writes
// sRGB, or PQ with scene and UI brightness scales when HDR output is on. Upscaling runs the TAA at
// render resolution and does that tail in CopyTemporalAA_PS instead. These shaders replace only
// the TAA itself: they read the game's own constant buffers at the registers the shipped shaders
// use, so vignette, grain, UI and HDR brightness stay exactly as authored.
//
// SDR: DLSS runs in LDR mode on the square-root encoding. HDR: highlights exceed 1, so the prepare
// pass squares the colour and DLSS runs in HDR mode on linear values; `Linear` marks its output.
namespace dgmp::upscaler::shaders
{
inline constexpr char Common[] = R"(
// Bend's YCoCg history encoding: Y, Co * 0.5 + 0.5, Cg * 0.5 + 0.5.
float3 ToYCoCg(float3 rgb)
{
    float y = rgb.r * 0.25 + rgb.g * 0.5 + rgb.b * 0.25;
    float co = rgb.r * 0.5 - rgb.b * 0.5;
    float cg = rgb.g * 0.5 - rgb.r * 0.25 - rgb.b * 0.25;
    return float3(y, co * 0.5 + 0.5, cg * 0.5 + 0.5);
}

float3 FromYCoCg(float3 ycocg)
{
    float co = ycocg.y * 2 - 1, cg = ycocg.z * 2 - 1;
    return float3(ycocg.x + co - cg, ycocg.x + cg, ycocg.x - co - cg);
}

// Multiplicative film grain; scale.x is the strength and scale.y its bias (0, 1 disables it).
float Grain(float2 position, float4 scale)
{
    return frac(sin(position.y * 543.31 + position.x) * 493013.0) * scale.x + scale.y;
}

// tint.rgb is added, tint.a scales the scene, shape.xy shape the falloff and edge.rgb fills it.
// `brightness` is the HDR scene scale (1 in SDR).
float3 Vignette(float3 color, float2 uv, float4 tint, float4 shape, float4 edge, float brightness)
{
    float2 offset = uv - 0.5;
    float amount = saturate(exp2((0.5 - dot(offset, offset)) * -shape.x) * shape.y);
    return color * ((tint.a - amount * tint.a) * brightness) + tint.rgb + edge.rgb * (amount * amount * tint.a);
}

float3 EncodeSrgb(float3 color)
{
    return min(1.055 * pow(max(color, 0.003131), 1.0 / 2.4) - 0.055, color * 12.92);
}

// Bend's HDR output: a soft toe into ST 2084, with 1.0 at 200 nits.
float3 EncodePq(float3 color)
{
    float3 x = color * 0.02 + (exp2(color * -40.96) - 1) * 0.00045;
    float3 p = pow(abs(x), 0.1593017578125);
    return pow((18.8515625 * p + 0.8359375) / (18.6875 * p + 1), 78.84375);
}

// Diagnostic views: 1 motion vectors, 2 depth, 3 DLSS input colour.
float3 DebugColor(uint view, float2 motion, float depth, float3 input)
{
    if (view == 1) return float3(saturate(0.5 + motion * 0.05), saturate(length(motion) * 0.05));
    if (view == 2) return saturate(depth * 50).xxx;
    return input;
}
)";

// Converts the game's packed motion, device depth and colour into DLSS inputs at render resolution.
inline constexpr char Prepare[] = R"(
cbuffer TemporalParameters : register(b0) { float4 Temporal[11]; };
cbuffer GlobalParameters : register(b1) { float4 Global[11]; };
cbuffer PrepareParameters : register(b2)
{
    uint2 RenderSize;
    uint UvRegister;      // .zw: reciprocal of the input buffer size
    uint HistoryRegister; // .xy scale the current UV, .zw map it to history UV
    uint CheckerRegister; // .x scales X for checkerboard layouts
    uint Linear;          // Square the colour for DLSS's HDR mode
    uint2 Padding;
};
Texture2D<uint> Motion : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float> Depth : register(t2);
RWTexture2D<float2> OutMotion : register(u0);
RWTexture2D<float> OutDepth : register(u1);
RWTexture2D<float4> OutColor : register(u2);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= RenderSize)) return;
    float2 inverseSize = Temporal[UvRegister].zw;
    float2 uv = (pixel + 0.5) * inverseSize;
    uint packed = Motion[pixel];
    float2 motion = 0;
    // Zero marks pixels without usable history (off-screen or too fast); the TAA rejects them too.
    if (packed != 0)
    {
        // 14-bit X and 13-bit Y, the same decode as TemporalAACS_T1X.
        float2 encoded = float2(packed & 0x3fff, (packed >> 14) & 0x1fff) * Global[10].zw;
        float4 history = Temporal[HistoryRegister];
        float2 previous = ((encoded - 0.5) * 0.25 + uv * history.xy * float2(Temporal[CheckerRegister].x, 1)) * history.zw;
        motion = (previous - uv) / inverseSize;
    }
    OutMotion[pixel] = motion;
    OutDepth[pixel] = Depth[pixel];
    float4 color = Color[pixel];
    OutColor[pixel] = Linear ? float4(color.rgb * color.rgb, color.a) : color;
}
)";

// Replaces CopyTemporalAA_PS (TBendSMAABufferCopyPS_00/_01, HDR _10/_11) while DLSS upscales.
inline constexpr char CompositePixel[] = R"(
cbuffer CompositeParameters : register(b0) { float4 Composite[7]; };
cbuffer OutputParameters : register(b1)
{
    int2 OutputOrigin;
    uint DebugView;
    uint Hdr;     // The HDR variant: brightness in Composite[5].xy, blend flag in .z, PQ output
    float2 RenderScale;
    uint Linear;  // DLSS output is linear rather than square-root encoded
    uint Padding;
};
Texture2D<float4> Interface : register(t0);
Texture2D<float4> Upscaled : register(t1);
Texture2D<float4> Auxiliary : register(t2);
Texture2D<float2> MotionInput : register(t3);
Texture2D<float> DepthInput : register(t4);
Texture2D<float4> ColorInput : register(t5);
SamplerState Bilinear : register(s0);

float4 main(float2 uv : TEXCOORD0, float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    int2 local = pixel - OutputOrigin;
    float3 color = Upscaled.Load(int3(local, 0)).rgb;
    float3 encoded = Linear ? sqrt(max(color, 0)) : color;
    if (DebugView != 0)
    {
        int3 source = int3(int2(local * RenderScale), 0);
        encoded = DebugColor(DebugView, MotionInput.Load(source), DepthInput.Load(source), ColorInput.Load(source).rgb);
        color = encoded * encoded;
    }
    else if (asuint(Hdr ? Composite[5].z : Composite[5].x) != 0)
    {
        float3 auxiliary = Auxiliary.SampleLevel(Bilinear, uv * Composite[6].xy * Composite[6].zw, 0).xyz;
        encoded = FromYCoCg(auxiliary + (ToYCoCg(encoded) - auxiliary) * 0.25);
        color = encoded * encoded;
    }
    else if (!Linear) color = encoded * encoded;
    color = Vignette(color, uv, Composite[2], Composite[3], Composite[4], Hdr ? Composite[5].x : 1)
        * Grain(uv + Composite[0].xy, Composite[1]);
    float4 ui = Interface.Load(int3(pixel, 0));
    color = color * (1 - ui.a) + ui.rgb * (Hdr ? Composite[5].y : 1);
    return float4(Hdr ? EncodePq(color) : EncodeSrgb(color), 0);
}
)";

// Replaces the tail of TBendSMAATemporalAACS_T1X_0000 (HDR: _0010) when DLSS runs at output
// resolution (DLAA). The HDR variant keeps its brightness in Temporal[5] and shifts the rest by one.
inline constexpr char CompositeCompute[] = R"(
cbuffer TemporalParameters : register(b0) { float4 Temporal[8]; };
cbuffer OutputParameters : register(b1)
{
    uint2 OutputSize;
    uint DebugView;
    uint Hdr;
    uint Linear;
    uint3 Padding;
};
Texture2D<float4> Interface : register(t0);
Texture2D<float4> Upscaled : register(t1);
Texture2D<float2> MotionInput : register(t3);
Texture2D<float> DepthInput : register(t4);
Texture2D<float4> ColorInput : register(t5);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= OutputSize)) return;
    float2 center = pixel + 0.5;
    float3 color = Upscaled[pixel].rgb;
    bool linearColor = Linear != 0;
    if (DebugView != 0)
    {
        color = DebugColor(DebugView, MotionInput[pixel], DepthInput[pixel], ColorInput[pixel].rgb);
        linearColor = false;
    }
    // The full-resolution TAA applies grain before and after decoding; keep both.
    float grain = Grain(center * Temporal[Hdr ? 6 : 5].zw + Temporal[0].xy, Temporal[1]);
    color = linearColor ? color * (grain * grain) : (color * grain) * (color * grain);
    float2 uv = center * Temporal[Hdr ? 7 : 6].zw;
    color = Vignette(color, uv, Temporal[2], Temporal[3], Temporal[4], Hdr ? Temporal[5].x : 1) * Grain(uv + Temporal[0].xy, Temporal[1]);
    float4 ui = Interface[pixel];
    color = color * (1 - ui.a) + ui.rgb * (Hdr ? Temporal[5].y : 1);
    float3 encoded = Hdr ? EncodePq(color) : EncodeSrgb(color);
    Output[pixel] = float4(encoded, encoded.r);
}
)";
}
