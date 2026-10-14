// Traces one ray per render pixel through the captured geometry (cs_6_5, compiled by dxc at build
// time). Rays follow the game's jittered projection, so hits line up with its rasterised pixels.
// A second ray from the surface the game rendered tests whether the sun reaches it, through the
// view's geometry and the casters of the game's sun shadow map. Alpha-tested triangles are
// non-opaque: a hit on one counts only where its material's baked coverage is set. Sun rays also see
// the static geometry of earlier frames that the view did not draw this frame.
#define TraceRootSignature "RootConstants(num32BitConstants=48, b0), SRV(t0), SRV(t1), SRV(t2), SRV(t6), SRV(t7), SRV(t8), " \
    "DescriptorTable(SRV(t3, numDescriptors=3), UAV(u0, numDescriptors=2), SRV(t9, numDescriptors=2))"

cbuffer TraceParameters : register(b0)
{
    row_major float4x4 WorldFromClip;
    row_major float4x4 ClipFromWorld;
    uint2 Size;
    uint2 MaskedBase;   // Per instance: its first alpha-tested vertex in Coordinates
    float4 Sun;         // xyz toward the sun; w is 0 until the game's shadow pass has been seen
    // Per instance, the view's geometry then the shadow casters: the first vertex of each of its
    // geometries (opaque, then alpha-tested, each only when present).
    uint4 GeometryBase;
    uint MaterialDraws; // Entries in Materials
    uint PrimaryMask;   // The instances primary rays see: the view's geometry (1) or the shadow casters (2)
    uint OffscreenCheck; // Also trace the sun through what survives off screen: casters and cached geometry
    uint Padding;
};
RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<float4> Vertices : register(t1);
StructuredBuffer<uint2> Materials : register(t2); // Per alpha-tested draw: first vertex, coverage slot
Texture2D<float> GameDepth : register(t3);        // The game's device depth over the view, 0 for sky
Texture2D<float4> Coordinates : register(t4);     // Alpha-tested vertices' texture coordinates and fade, 4096 a row
Texture2D<float> Coverage : register(t5);         // Each material's lowest passing fade level * 16 / 255; 1: never
RWTexture2D<float4> Output : register(u0);        // rgb: shaded geometry, a: device depth of the hit (0: miss)
RWTexture2D<float4> SunOutput : register(u1);     // r: sun visibility, g: N.L, b: 1 where the normal is known,
                                                  // a: the visibility through what survives off screen (OffscreenCheck)
StructuredBuffer<uint2> CachedDraws : register(t6);       // Per cached instance (from 2): first coordinate, coverage slot
StructuredBuffer<float4> CacheCoordinates : register(t7); // Cached alpha-tested vertices' texture coordinates and fade
StructuredBuffer<float4> TemplateCoordinates : register(t8); // Those of the foliage templates (slots with the top bit set)
// The cached draws' and foliage templates' vertices: an instance's first is its InstanceID.
StructuredBuffer<float4> CacheVertices : register(t9);
StructuredBuffer<float4> TemplateVertices : register(t10);

#include "ray_tracing_scene.hlsli"

float3 Unproject(float2 screen, float depth)
{
    float4 world = mul(float4(screen, depth, 1), WorldFromClip);
    return world.xyz / world.w;
}

// 1 where no captured geometry of the given instances lies between the point and the sun. The
// instanced foliage (mask 4) repeats the trees on screen, wind and level of detail aside, so it only
// occludes from `near` on: its copy of a tree never shadows that tree's own leaves.
float TraceSun(float3 start, uint mask, float near)
{
    if (Occluded(start, Sun.xyz, mask & 3, 0, 1e30)) return 0;
    return (mask & 4) && Occluded(start, Sun.xyz, 4, near, 1e30) ? 0 : 1;
}

[RootSignature(TraceRootSignature)]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    float2 screen = (pixel + 0.5) / Size * float2(2, -2) + float2(-1, 1);
    // Reversed Z with an infinite far plane: 1 is the near plane, 0.5 twice as far.
    float3 eye = Unproject(screen, 1);
    RayDesc ray;
    ray.Origin = eye;
    ray.Direction = normalize(Unproject(screen, 0.5) - eye);
    ray.TMin = 0;
    ray.TMax = 1e30;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, PrimaryMask, ray);
    Resolve(query);
    bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    float3 normal = 0;
    float hitDepth = 0;
    if (hit)
    {
        // Cached draws and tree copies (instances from 2) keep their vertices in their caches, in their
        // instance's space.
        uint instance = query.CommittedInstanceIndex();
        float3 a, b, c;
        if (instance >= 2)
        {
            uint first = query.CommittedInstanceID() + query.CommittedPrimitiveIndex() * 3;
            if (CachedDraws[instance - 2].y >> 31)
            {
                a = TemplateVertices[first].xyz;
                b = TemplateVertices[first + 1].xyz;
                c = TemplateVertices[first + 2].xyz;
            }
            else
            {
                a = CacheVertices[first].xyz;
                b = CacheVertices[first + 1].xyz;
                c = CacheVertices[first + 2].xyz;
            }
        }
        else
        {
            uint first = GeometryBase[instance * 2 + query.CommittedGeometryIndex()] + query.CommittedPrimitiveIndex() * 3;
            a = Vertices[first].xyz;
            b = Vertices[first + 1].xyz;
            c = Vertices[first + 2].xyz;
        }
        normal = cross(b - a, c - a);
        if (instance >= 2) normal = mul((float3x3)query.CommittedObjectToWorld3x4(), normal);
        normal = normalize(normal);
        if (dot(normal, ray.Direction) > 0) normal = -normal;
        float4 clip = mul(float4(ray.Origin + ray.Direction * query.CommittedRayT(), 1), ClipFromWorld);
        hitDepth = clip.z / clip.w;
        float facing = saturate(dot(normal, -ray.Direction));
        Output[pixel] = float4((normal * 0.5 + 0.5) * (0.25 + 0.75 * facing), hitDepth);
    }
    else Output[pixel] = 0;

    // The sun is tested from the surface the game rendered, using the traced normal where both are
    // the same surface.
    float game = GameDepth[pixel];
    float depth = game > 0 ? game : hitDepth;
    if (depth <= 0 || Sun.w == 0)
    {
        SunOutput[pixel] = float4(1, 0, 0, 0);
        return;
    }

    float3 surface = Unproject(screen, depth);
    bool known = hit && abs(hitDepth / depth - 1) < 0.01;
    // Float precision falls with distance, so the start moves further off the surface.
    float bias = 0.5 + length(surface - eye) * 1e-3;
    float3 start = surface + (known ? normal : Sun.xyz) * bias;
    float lit = known ? dot(normal, Sun.xyz) : 1;
    // Shadows of geometry that only the view's capture holds go when it leaves the view. A caster
    // closer to the pixel than the foliage's `near` leaves the view with it, bar the screen's edges,
    // so the check keeps the view's geometry that close.
    float near = 30 + length(surface - eye) * 2e-3;
    float offscreen = OffscreenCheck && !Occluded(start, Sun.xyz, 1, 0, near) ? TraceSun(start, 6, near) : 0;
    SunOutput[pixel] = float4(TraceSun(start, 7, near), lit, known, offscreen);
}
