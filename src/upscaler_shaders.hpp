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
//
// GAME_VERSION (107 or 113) selects the game build's composite: 1.07 has no film grain after the
// vignette, a simpler upscale composite and a different HDR encoding (docs/DIFFERENCES.md).
namespace days_gone_dlss::upscaler::shaders
{
inline constexpr char Common[] = R"(
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

#if GAME_VERSION == 107
// Bend's 1.07 HDR output: a log fit of ST 2084 with 1.0 at 200 nits and a quadratic toe.
float3 EncodeHdr(float3 color)
{
    float3 curve = saturate(log2(color * 0.5 + 0.00263665011) * 0.0765511990 + 0.655820012);
    float3 toe = saturate(0.141420007 - color * 0.353549987);
    return saturate(color * 16384) * (toe * toe + curve);
}
#else
// Bend's HDR output: a soft toe into ST 2084, with 1.0 at 200 nits.
float3 EncodeHdr(float3 color)
{
    float3 x = color * 0.02 + (exp2(color * -40.96) - 1) * 0.00045;
    float3 p = pow(abs(x), 0.1593017578125);
    return pow((18.8515625 * p + 0.8359375) / (18.6875 * p + 1), 78.84375);
}
#endif

// AMD FidelityFX RCAS (FsrRcasF in ffx_fsr1.h, MIT: AMD-FidelityFX-LICENSE.txt) on square-root encoded
// colour: b, d, f and h are the pixels above, left, right and below e. Its noise filter is left out
// because grain is applied afterwards. RCAS limits sharpening to the 0 to 1 range, which HDR exceeds,
// so HDR is sharpened as x / (1 + x). Sharpness scales the negative lobe; 0 would return e.
float3 RcasIn(float3 x, bool hdr) { return hdr ? x / (1 + x) : saturate(x); }

float3 Rcas(float3 b, float3 d, float3 e, float3 f, float3 h, float sharpness, bool hdr)
{
    b = RcasIn(b, hdr); d = RcasIn(d, hdr); e = RcasIn(e, hdr); f = RcasIn(f, hdr); h = RcasIn(h, hdr);
    float3 low = min(min(b, d), min(f, h)), high = max(max(b, d), max(f, h));
    // The lobe at which each channel would clip below 0 or above 1.
    float3 hitMin = min(low, e) / max(4 * high, 1.0 / 65536);
    float3 hitMax = (1 - max(high, e)) / min(4 * low - 4, -1.0 / 65536);
    float3 lobes = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16), min(max(lobes.r, max(lobes.g, lobes.b)), 0)) * sharpness;
    float3 result = (lobe * (b + d + f + h) + e) / (4 * lobe + 1);
    return hdr ? result / (1 - min(result, 0.99999)) : result;
}

// Diagnostic views: 1 motion vectors, 2 depth, 3 DLSS input colour, 4 current-colour bias in red.
float3 DebugColor(uint view, float2 motion, float depth, float3 input, float bias)
{
    if (view == 1) return float3(saturate(0.5 + motion * 0.05), saturate(length(motion) * 0.05));
    if (view == 2) return saturate(depth * 50).xxx;
    if (view == 4) return lerp(saturate(input) * 0.5, float3(1, 0, 0), bias);
    return input;
}
)";

// Runs beside FDepthResolveCS on its inputs: device depth, GBuffer velocity and its reprojection.
// The resolve packs motion for the game's TAA, dilated to the nearest depth in 3x3, quantized to
// about 0.06 pixels at 4K and zero where it is off-screen or fast. DLSS shows each of those as
// warping, so this repeats the resolve's motion maths per pixel in full precision.
//
// The resolve also has the TAA drop history where a pixel is marked as moving (bit 31 of t0) but
// drew no object velocity, such as a character the velocity pass skipped. DLSS gets the same
// pixels as its current-colour bias.
inline constexpr char Motion[] = R"(
cbuffer ResolveParameters : register(b0) { float4 Resolve[13]; };
Texture2D<uint> Pixels : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float2> Velocity : register(t3);
RWTexture2D<float2> OutMotion : register(u0);
RWTexture2D<float> OutDepth : register(u1);
RWTexture2D<float> OutBias : register(u2);

// UE4's DecodeVelocityFromTexture; Bend keeps a flag in the lowest bit of Y.
float2 DecodeVelocity(float2 encoded)
{
    const float scale = 1 / (0.499 * 0.5);
    float y = (uint(encoded.y * 65535 + 0.5) & ~1u) - 0.5;
    return float2(encoded.x, y / 65535) * scale - 32767.0 / 65535 * scale;
}

[numthreads(8, 8, 1)]
void main(uint2 thread : SV_DispatchThreadID)
{
    int4 view = asint(Resolve[7]); // Min and max in buffer pixels
    int2 pixel = int2(thread) + view.xy;
    if (any(pixel >= view.zw)) return;
    // .y drops all motion, .z drops object velocity and, unless .w, camera motion too.
    uint4 flags = asuint(Resolve[5]);
    float depth = Depth[pixel];
    float2 velocity = Velocity[pixel];
    float2 screen = pixel * Resolve[8].xy + Resolve[8].zw;
    bool objectVelocity = velocity.x > 0 && !flags.y && !flags.z;
    float2 motion = 0; // Screen position delta from this frame to the previous one
    if (objectVelocity) motion = -DecodeVelocity(velocity);
    else if (!flags.y && (!flags.z || flags.w))
    {
        float4 previous = screen.x * Resolve[9] + screen.y * Resolve[10] + max(depth, 1e-11) * Resolve[11] + Resolve[12];
        motion = previous.xy / previous.w - screen;
    }
    // Back to render pixels through the inverse of the pixel to screen mapping.
    OutMotion[thread] = motion / Resolve[8].xy;
    OutDepth[thread] = depth;
    OutBias[thread] = (Pixels[pixel] & 0x80000000) && !objectVelocity && !flags.z ? 1 : 0;
}
)";

// Converts the game's colour into the DLSS input at render resolution.
inline constexpr char Prepare[] = R"(
cbuffer PrepareParameters : register(b0)
{
    uint2 RenderSize;
    uint Linear; // Square the colour for DLSS's HDR mode
    uint Padding;
};
Texture2D<float4> Color : register(t0);
RWTexture2D<float4> OutColor : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= RenderSize)) return;
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
    uint Hdr;     // The HDR variant: brightness constants (1.13: Composite[5], 1.07: [3]) and HDR output
    float2 RenderScale;
    uint Linear;  // DLSS output is linear rather than square-root encoded
    float Sharpness;
    uint Filtered; // _01/_11, which filter the history: 1.13 sharpens it, 1.07 resamples it about the centre
};
Texture2D<float4> Interface : register(t0);
Texture2D<float4> Upscaled : register(t1);
Texture2D<float2> MotionInput : register(t2);
Texture2D<float> DepthInput : register(t3);
Texture2D<float4> ColorInput : register(t4);
Texture2D<float> BiasInput : register(t5);

float3 Encoded(int2 local, int2 last)
{
    float3 color = Upscaled.Load(int3(clamp(local, 0, last), 0)).rgb;
    return Linear ? sqrt(max(color, 0)) : color;
}

// The second target, when bound, takes the frame before the UI for frame generation.
struct Output
{
    float4 color : SV_Target0;
    float4 hudless : SV_Target1;
};

Output main(float2 uv : TEXCOORD0, float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    int2 local = pixel - OutputOrigin;
    uint width, height;
    Upscaled.GetDimensions(width, height);
    int2 last = int2(width, height) - 1;
    float3 encoded = Encoded(local, last);
    if (Sharpness > 0 && DebugView == 0)
        encoded = Rcas(Encoded(local + int2(0, -1), last), Encoded(local + int2(-1, 0), last), encoded,
            Encoded(local + int2(1, 0), last), Encoded(local + int2(0, 1), last), Sharpness, Linear != 0);
    float3 color = encoded * encoded;
    if (DebugView != 0)
    {
        int3 source = int3(int2(local * RenderScale), 0);
        encoded = DebugColor(DebugView, MotionInput.Load(source), DepthInput.Load(source), ColorInput.Load(source).rgb,
            BiasInput.Load(source));
        color = encoded * encoded;
    }
#if GAME_VERSION == 107
    // No auxiliary history or grain. The filtering variants sample the history and the vignette at
    // uv scaled about the centre by 1 - 4a, with a = 1 / history size - 1 / output size from the two
    // rows after the brightness. Only the vignette follows: DLSS output is already at output size.
    float2 vignetteUv = uv;
    if (Filtered)
    {
        uint row = Hdr ? 4 : 3;
        float2 a = (1 - Composite[row + 1].zw * Composite[row].xy) * Composite[row].zw;
        vignetteUv = (uv - 0.5) * (1 - 4 * a) + 0.5;
    }
    color = Vignette(color, vignetteUv, Composite[0], Composite[1], Composite[2], Hdr ? Composite[3].x : 1);
    float interfaceBrightness = Hdr ? Composite[3].y : 1;
#else
    // When row 5 flags it, e.g. on every cutscene cut, the game blends in 75% of an auxiliary history
    // that its own TAA writes. That TAA does not run under DLSS, so the blend would show a stale
    // texture as a white flash; leave it out.
    color = Vignette(color, uv, Composite[2], Composite[3], Composite[4], Hdr ? Composite[5].x : 1)
        * Grain(uv + Composite[0].xy, Composite[1]);
    float interfaceBrightness = Hdr ? Composite[5].y : 1;
#endif
    Output result;
    result.hudless = float4(Hdr ? EncodeHdr(color) : EncodeSrgb(color), 0);
    float4 ui = Interface.Load(int3(pixel, 0));
    color = color * (1 - ui.a) + ui.rgb * interfaceBrightness;
    result.color = float4(Hdr ? EncodeHdr(color) : EncodeSrgb(color), 0);
    return result;
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
    float Sharpness;
    uint2 Padding;
};
Texture2D<float4> Interface : register(t0);
Texture2D<float4> Upscaled : register(t1);
Texture2D<float2> MotionInput : register(t3);
Texture2D<float> DepthInput : register(t4);
Texture2D<float4> ColorInput : register(t5);
Texture2D<float> BiasInput : register(t6);
RWTexture2D<float4> Output : register(u0);
// When bound, the frame before the UI for frame generation.
RWTexture2D<float4> Hudless : register(u1);

float3 Encoded(int2 pixel)
{
    float3 color = Upscaled[clamp(pixel, 0, int2(OutputSize) - 1)].rgb;
    return Linear ? sqrt(max(color, 0)) : color;
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= OutputSize)) return;
    float2 center = pixel + 0.5;
    float3 color = Upscaled[pixel].rgb;
    bool linearColor = Linear != 0;
    if (DebugView != 0)
    {
        color = DebugColor(DebugView, MotionInput[pixel], DepthInput[pixel], ColorInput[pixel].rgb, BiasInput[pixel]);
        linearColor = false;
    }
    else if (Sharpness > 0)
    {
        int2 at = int2(pixel);
        float3 encoded = Rcas(Encoded(at + int2(0, -1)), Encoded(at + int2(-1, 0)), Encoded(at),
            Encoded(at + int2(1, 0)), Encoded(at + int2(0, 1)), Sharpness, linearColor);
        color = linearColor ? encoded * encoded : encoded;
    }
    // The full-resolution TAA applies grain before decoding, and 1.13 again after the vignette.
    float grain = Grain(center * Temporal[Hdr ? 6 : 5].zw + Temporal[0].xy, Temporal[1]);
    color = linearColor ? color * (grain * grain) : (color * grain) * (color * grain);
    float2 uv = center * Temporal[Hdr ? 7 : 6].zw;
    color = Vignette(color, uv, Temporal[2], Temporal[3], Temporal[4], Hdr ? Temporal[5].x : 1);
#if GAME_VERSION != 107
    color *= Grain(uv + Temporal[0].xy, Temporal[1]);
#endif
    Hudless[pixel] = float4(Hdr ? EncodeHdr(color) : EncodeSrgb(color), 0);
    float4 ui = Interface[pixel];
    color = color * (1 - ui.a) + ui.rgb * (Hdr ? Temporal[5].y : 1);
    float3 encoded = Hdr ? EncodeHdr(color) : EncodeSrgb(color);
    Output[pixel] = float4(encoded, encoded.r);
}
)";
}
