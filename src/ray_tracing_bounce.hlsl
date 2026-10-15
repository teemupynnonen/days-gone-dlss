// Traces the sun's light bounced once off the captured geometry: global illumination (cs_6_5,
// compiled by dxc at build time). Each pixel of the game's surface sends one cosine-weighted ray
// into the hemisphere above it. Where it hits, a ray toward the sun tests whether the sun lights
// that point, and the point's diffuse colour comes from the game's G-buffer when the game drew the
// same point on screen, a mid grey otherwise. The output is the light bounced toward the pixel per
// unit of the sun's colour; Direct3D 11 accumulates it over frames, filters it and lights the scene
// with it.
#define BounceRootSignature "RootConstants(num32BitConstants=45, b0), SRV(t0), SRV(t1), SRV(t6), SRV(t7), SRV(t8), SRV(t9), " \
    "SRV(t10), DescriptorTable(SRV(t3, numDescriptors=3), SRV(t11, numDescriptors=2), UAV(u0))"

cbuffer BounceParameters : register(b0)
{
    row_major float4x4 WorldFromClip;
    row_major float4x4 ClipFromWorld;
    uint2 Size;
    uint2 MaskedBase;   // Per instance: its first alpha-tested vertex in Coordinates
    float4 Sun;         // xyz toward the sun; w is 0 until the game's shadow pass has been seen
    uint4 GeometryBase; // Per instance, the view's geometry then the shadow casters: the first vertex of each geometry
    uint Frame;         // Varies the rays from frame to frame
};
RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<float4> Vertices : register(t1);
Texture2D<float> GameDepth : register(t3);        // The game's device depth over the view, 0 for sky
// Alpha-tested vertices' texture coordinates, fade and their draw's coverage slot (w, -1: none), 4096 a row
Texture2D<float4> Coordinates : register(t4);
Texture2D<float> Coverage : register(t5);         // Each material's lowest passing fade level * 16 / 255; 1: never
StructuredBuffer<uint2> CachedDraws : register(t6);       // Per cached instance (from 2): first coordinate, coverage slot
StructuredBuffer<float4> CacheCoordinates : register(t7); // Cached alpha-tested vertices' texture coordinates and fade
StructuredBuffer<float4> TemplateCoordinates : register(t8); // Those of the foliage templates (slots with the top bit set)
// The cached draws' and foliage templates' vertices: an instance's first is its InstanceID.
StructuredBuffer<float4> CacheVertices : register(t9);
StructuredBuffer<float4> TemplateVertices : register(t10);
Texture2D<uint> Normals : register(t11);  // The game's G-buffer normal over the view, 0 for sky
Texture2D<float4> Albedo : register(t12); // rgb: the game's diffuse colour over the view, a: 1 where known
RWTexture2D<float4> Output : register(u0); // rgb: bounced light per unit of sun colour, a: 1 on the game's surfaces

#include "ray_tracing_scene.hlsli"

// Bounce rays reach 50 m. The shadow casters and the instanced foliage repeat geometry the view
// drew, at another level of detail or wind; they count from a metre and 1% of the view distance on,
// so a surface does not light itself from its own copy.
static const float MaxDistance = 5000, CopyDistance = 100, CopyDistancePerDistance = 0.01;
// The diffuse colour of points the game did not draw: about that of earth, rock and bark.
static const float3 UnknownAlbedo = 0.25;

float3 Unproject(float2 screen, float depth)
{
    float4 world = mul(float4(screen, depth, 1), WorldFromClip);
    return world.xyz / world.w;
}

float3 CosineDirection(float3 normal, uint seed)
{
    uint a = Hash(seed), b = Hash(a);
    float2 random = float2(a, b) / 4294967296.0;
    // An orthonormal basis without branches (Duff et al. 2017).
    float flip = normal.z >= 0 ? 1 : -1;
    float c = -1 / (flip + normal.z), d = normal.x * normal.y * c;
    float3 tangent = float3(1 + flip * normal.x * normal.x * c, flip * d, -flip * normal.x);
    float3 bitangent = float3(d, flip + normal.y * normal.y * c, -normal.y);
    float radius = sqrt(random.x), angle = 6.2831853 * random.y;
    return (tangent * cos(angle) + bitangent * sin(angle)) * radius + normal * sqrt(1 - random.x);
}

// The normal of the committed triangle. This frame's captures (instances 0 and 1) are in Vertices in
// world space; cached draws and foliage keep theirs in their caches, placed by their instance.
float3 HitNormal(inout RayQuery<RAY_FLAG_NONE> query)
{
    uint instance = query.CommittedInstanceIndex(), first = query.CommittedPrimitiveIndex() * 3;
    if (instance < 2)
    {
        first += GeometryBase[instance * 2 + query.CommittedGeometryIndex()];
        float3 a = Vertices[first].xyz, b = Vertices[first + 1].xyz, c = Vertices[first + 2].xyz;
        return normalize(cross(b - a, c - a));
    }

    first += query.CommittedInstanceID();
    bool foliage = CachedDraws[instance - 2].y >> 31;
    float3 a = foliage ? TemplateVertices[first].xyz : CacheVertices[first].xyz;
    float3 b = foliage ? TemplateVertices[first + 1].xyz : CacheVertices[first + 1].xyz;
    float3 c = foliage ? TemplateVertices[first + 2].xyz : CacheVertices[first + 2].xyz;
    return normalize(mul((float3x3)query.CommittedObjectToWorld3x4(), cross(b - a, c - a)));
}

// The game's diffuse colour where it drew the point, if it did.
float3 AlbedoAt(float3 position)
{
    float4 clip = mul(float4(position, 1), ClipFromWorld);
    float2 screen = clip.xy / clip.w;
    if (clip.w <= 0 || any(abs(screen) >= 1)) return UnknownAlbedo;

    uint2 texel = min(uint2((screen * float2(0.5, -0.5) + 0.5) * Size), Size - 1);
    float4 albedo = Albedo[texel];
    // Reversed-Z device depth is proportional to 1 / view depth.
    bool same = abs(GameDepth[texel] * clip.w / clip.z - 1) < 0.02;
    return same && albedo.a > 0 ? albedo.rgb : UnknownAlbedo;
}

[RootSignature(BounceRootSignature)]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    float depth = GameDepth[pixel];
    uint packedNormal = Normals[pixel];
    if (depth <= 0 || !packedNormal)
    {
        Output[pixel] = 0;
        return;
    }
    if (Sun.w == 0)
    {
        Output[pixel] = float4(0, 0, 0, 1);
        return;
    }

    float2 screen = (pixel + 0.5) / Size * float2(2, -2) + float2(-1, 1);
    float3 eye = Unproject(screen, 1), surface = Unproject(screen, depth), normal = DecodeNormal(packedNormal);
    float viewDistance = length(surface - eye);
    // Float precision falls with distance, so the start moves further off the surface. Moving toward
    // the eye as well keeps it in front of the surface where the G-buffer's normal tilts into it.
    float bias = 0.5 + viewDistance * 1e-3;
    RayDesc ray;
    ray.Origin = surface + (normal + (eye - surface) / viewDistance) * bias;
    ray.Direction = CosineDirection(normal, Hash(pixel.x + Hash(pixel.y + Hash(Frame))));
    ray.TMin = 0;
    ray.TMax = MaxDistance;
    RayQuery<RAY_FLAG_NONE> view;
    view.TraceRayInline(Scene, RAY_FLAG_NONE, 1, ray);
    Resolve(view);
    if (view.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ray.TMax = view.CommittedRayT();
    ray.TMin = CopyDistance + CopyDistancePerDistance * viewDistance;
    RayQuery<RAY_FLAG_NONE> copies;
    if (ray.TMin < ray.TMax)
    {
        copies.TraceRayInline(Scene, RAY_FLAG_NONE, 14, ray);
        Resolve(copies);
    }
    bool hitCopy = ray.TMin < ray.TMax && copies.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    if (!hitCopy && view.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        Output[pixel] = float4(0, 0, 0, 1);
        return;
    }

    float3 hit = ray.Origin + ray.Direction * (hitCopy ? copies.CommittedRayT() : view.CommittedRayT());
    float3 hitNormal = hitCopy ? HitNormal(copies) : HitNormal(view);
    if (dot(hitNormal, ray.Direction) > 0) hitNormal = -hitNormal;
    float lit = saturate(dot(hitNormal, Sun.xyz));
    // Everything captured shadows the point, foliage included: a tree's copy shading its own leaves
    // is canopy shading them all the same.
    float3 start = hit + hitNormal * (0.5 + length(hit - eye) * 1e-3);
    if (lit > 0 && Occluded(start, Sun.xyz, 15, 0, 1e30)) lit = 0;
    Output[pixel] = float4(AlbedoAt(hit) * lit, 1);
}
