// Traces the game's local lights (cs_6_5, compiled by dxc at build time). The game has drawn each
// light without its shadow into a slot of its own. From each pixel a slot lights, rays toward the
// light decide how much of it reaches the pixel: one at its centre, or with soft shadows a few at
// points across its size. With bounce light, one cosine-weighted ray per pixel finds the surface the
// lights light in that direction, as the sun's bounce does.
#define LightsRootSignature "RootConstants(num32BitConstants=48, b0), SRV(t0), SRV(t2), SRV(t3), SRV(t4), " \
    "DescriptorTable(SRV(t5, numDescriptors=25), UAV(u0, numDescriptors=3))"

static const uint MaxLights = 16;
// A light's fixture (a lamp's glass, the logs of a fire) would shadow it, so rays stop this far (cm)
// short of the light, or its source radius if larger.
static const float FixtureMargin = 20;
// Soft shadows: rays per light and pixel. Lights the game gives no size get one: point lights (fires,
// bulbs) 2.5% of their reach, spot lights (lamps, the flashlight, headlights) 5 cm.
static const uint SoftSamples = 2;
static const float PointSizePerReach = 0.025, SpotSize = 5;
// Bounce rays reach 50 m. The cache and the trees' copies count from a metre and 1% of the view
// distance on, so a surface does not light itself from its own copy. Where a bounce ray lands, a light
// counts as at least 50 cm away: closer, its inverse square makes the odd ray that lands beside a
// fire hundreds of times brighter than its neighbours.
static const float BounceDistance = 5000, CopyDistance = 100, CopyDistancePerDistance = 0.01, BounceNearest = 50;
// The diffuse colour of points the game did not draw: about that of earth, rock and bark.
static const float3 UnknownAlbedo = 0.25;
static const uint Soft = 1, Bounce = 2;

cbuffer LightParameters : register(b0)
{
    row_major float4x4 WorldFromClip;
    row_major float4x4 ClipFromWorld;
    uint2 Size;
    uint2 MaskedBase;     // Per instance: its first alpha-tested vertex in Coordinates
    int2 Origin;          // The view's first pixel in the slots, which cover the game's scene colour
    uint LightCount;
    uint Frame;           // Varies the rays from frame to frame
    float3 Translation;   // World to the translated world the geometry is in
    uint Flags;           // Soft, Bounce
    uint4 GeometryBase;   // Per instance, the view's geometry then the sun's shadow casters: the first vertex of each geometry
};
RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<uint2> CachedDraws : register(t2);         // Per cached instance (from 2): first coordinate, coverage slot
StructuredBuffer<float4> CacheCoordinates : register(t3);    // Cached alpha-tested vertices' texture coordinates and fade
StructuredBuffer<float4> TemplateCoordinates : register(t4); // Those of the foliage templates (slots with the top bit set)
Texture2D<float> GameDepth : register(t5);                   // The game's device depth over the view, 0 for sky
// Alpha-tested vertices' texture coordinates, fade and their draw's coverage slot (w, -1 for none), 4096 a row
Texture2D<float4> Coordinates : register(t6);
Texture2D<float> Coverage : register(t7);                    // Each material's lowest passing fade level * 16 / 255; 1: never
// A row per slot: the light's constants as the game keeps them (position in world and inverse radius,
// colour, direction back toward the light, spot angles and source radius and length), then its
// scissor rectangle. A last row holds the lighting constants' row 43: y is the exposure.
Texture2D<float4> Lights : register(t8);
Texture2D<float3> Slots[MaxLights] : register(t9);           // Each light as the game drew it, unshadowed
Texture2D<uint> Normals : register(t25);                     // The game's G-buffer normal over the view, 0 for sky
Texture2D<float4> Albedo : register(t26);                    // rgb: the game's diffuse colour over the view, a: 1 where known
StructuredBuffer<float4> Vertices : register(t27);           // This frame's captures, in world space
// The cached draws' and foliage templates' vertices: an instance's first is its InstanceID.
StructuredBuffer<float4> CacheVertices : register(t28);
StructuredBuffer<float4> TemplateVertices : register(t29);
RWTexture2D<float4> Visibility : register(u0); // rgb: the share of the lights that reaches the pixel, a: 1 where any lights it
RWTexture2D<float4> Unshadowed : register(u1); // The lights the game drew, summed
RWTexture2D<float4> Bounced : register(u2);    // rgb: light bounced toward the pixel over its diffuse colour, a: 1 on the game's surfaces

#include "ray_tracing_scene.hlsli"

float3 Unproject(float2 screen, float depth)
{
    float4 world = mul(float4(screen, depth, 1), WorldFromClip);
    return world.xyz / world.w;
}

float3 CosineDirection(float3 normal, uint seed)
{
    float2 random = RandomPair(seed);
    float3 tangent, bitangent;
    Basis(normal, tangent, bitangent);
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

float3 LightPosition(uint light)
{
    return Lights[uint2(0, light)].xyz + Translation;
}

// Whether captured geometry lies between the point and `target` on the light, short of its fixture:
// the view's geometry and the cache (3), and the trees' copies from `near` (4). Not the sun shadow
// map's casters: coarser copies of moving objects, which would shadow the object the view drew.
bool Shadowed(float3 surface, float3 target, float margin, float bias, float near)
{
    float3 toTarget = target - surface;
    float targetDistance = length(toTarget);
    float3 direction = toTarget / targetDistance;
    float far = targetDistance - margin - bias;
    float3 start = surface + direction * bias;
    return far > 0 && (Occluded(start, direction, 3, 0, far) || (near < far && Occluded(start, direction, 4, near, far)));
}

// The share of a light that reaches the point: its centre, or with soft shadows points across its
// size, a sphere stretched along its direction by its source length.
float LightVisibility(uint light, float3 surface, float bias, float near, uint seed)
{
    float3 centre = LightPosition(light);
    float4 shape = Lights[uint2(3, light)];
    float margin = max(shape.z, FixtureMargin);
    if (!(Flags & Soft)) return Shadowed(surface, centre, margin, bias, near) ? 0 : 1;

    bool spot = shape.x > -1.5; // Point lights have spot angles of -2 and 1: every direction lights
    float radius = max(shape.z, spot ? SpotSize : PointSizePerReach / Lights[uint2(0, light)].w);
    float3 axis = Lights[uint2(2, light)].xyz;
    float3 tangent, bitangent;
    Basis(normalize(centre - surface), tangent, bitangent);
    float visible = 0;
    for (uint s = 0; s < SoftSamples; ++s)
    {
        float2 random = RandomPair(seed + s * 7919);
        float distance = sqrt(random.x) * radius, angle = 6.2831853 * random.y;
        float along = (RandomPair(seed + s * 7919 + 1).x - 0.5) * shape.w;
        float3 target = centre + (tangent * cos(angle) + bitangent * sin(angle)) * distance + axis * along;
        visible += Shadowed(surface, target, margin, bias, near) ? 0 : 1;
    }
    return visible / SoftSamples;
}

// The light the lights shine on a point facing `normal`, as Bend's lighting has it before the
// surface's colour: colour, inverse square falloff to the light's reach, spot cone and N.L, with the
// exposure. A ray toward each light's centre shadows it.
float3 Irradiance(float3 position, float3 normal, float bias, float near)
{
    float3 sum = 0;
    for (uint i = 0; i < LightCount; ++i)
    {
        float4 centre = Lights[uint2(0, i)];
        float3 toLight = centre.xyz + Translation - position;
        float squared = dot(toLight, toLight), reach = squared * centre.w * centre.w;
        if (reach >= 1) continue;

        float3 direction = toLight * rsqrt(squared);
        float facing = saturate(dot(normal, direction));
        float4 shape = Lights[uint2(3, i)];
        float spot = saturate((dot(direction, Lights[uint2(2, i)].xyz) - shape.x) * shape.y);
        float falloff = (1 - reach * reach) * (1 - reach * reach) / (max(squared, BounceNearest * BounceNearest) + 1);
        float3 light = Lights[uint2(1, i)].rgb * falloff * spot * spot * facing;
        if (any(light > 0) && !Shadowed(position, centre.xyz + Translation, max(shape.z, FixtureMargin), bias, near)) sum += light;
    }
    return sum * Lights[uint2(0, MaxLights)].y;
}

// The light bounced toward a pixel once, over the pixel's diffuse colour: one cosine-weighted ray,
// and the lights' irradiance where it hits times that point's colour over pi.
float3 BouncedLight(uint2 pixel, float3 surface, float3 normal)
{
    float viewDistance = length(surface);
    // Moving toward the eye as well keeps the start in front of the surface where the G-buffer's normal
    // tilts into it.
    float bias = 0.5 + viewDistance * 1e-3;
    RayDesc ray;
    ray.Origin = surface + (normal - surface / viewDistance) * bias;
    ray.Direction = CosineDirection(normal, Hash(pixel.x + Hash(pixel.y + Hash(Frame * 2 + 1))));
    ray.TMin = 0;
    ray.TMax = BounceDistance;
    RayQuery<RAY_FLAG_NONE> view;
    view.TraceRayInline(Scene, RAY_FLAG_NONE, 1, ray);
    Resolve(view);
    if (view.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ray.TMax = view.CommittedRayT();
    ray.TMin = CopyDistance + CopyDistancePerDistance * viewDistance;
    RayQuery<RAY_FLAG_NONE> copies;
    if (ray.TMin < ray.TMax)
    {
        copies.TraceRayInline(Scene, RAY_FLAG_NONE, 6, ray);
        Resolve(copies);
    }
    bool hitCopy = ray.TMin < ray.TMax && copies.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    if (!hitCopy && view.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0;

    float3 hit = ray.Origin + ray.Direction * (hitCopy ? copies.CommittedRayT() : view.CommittedRayT());
    float3 hitNormal = hitCopy ? HitNormal(copies) : HitNormal(view);
    if (dot(hitNormal, ray.Direction) > 0) hitNormal = -hitNormal;
    float hitDistance = length(hit);
    float3 start = hit + hitNormal * (0.5 + hitDistance * 1e-3);
    return AlbedoAt(hit) / 3.14159265 * Irradiance(start, hitNormal, 0.5 + hitDistance * 1e-3, 30 + hitDistance * 2e-3);
}

[RootSignature(LightsRootSignature)]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    float depth = GameDepth[pixel];
    int2 target = int2(pixel) + Origin;
    float3 lit = 0, unshadowed = 0;
    if (depth <= 0)
    {
        Visibility[pixel] = 0;
        Unshadowed[pixel] = 0;
        if (Flags & Bounce) Bounced[pixel] = 0;
        return;
    }

    float2 screen = (pixel + 0.5) / Size * float2(2, -2) + float2(-1, 1);
    float3 surface = Unproject(screen, depth);
    // The translated world has the camera at its origin. Float precision falls with distance, so rays
    // start further off the surface, as sun rays do; the trees' copies only occlude from `near`.
    float distance = length(surface);
    float bias = 0.5 + distance * 1e-3;
    float near = 30 + distance * 2e-3;
    uint seed = Hash(pixel.x + Hash(pixel.y + Hash(Frame * 2)));
    for (uint i = 0; i < LightCount; ++i)
    {
        int4 rect = asint(Lights[uint2(4, i)]);
        if (any(target < rect.xy) || any(target >= rect.zw)) continue;
        float3 light = Slots[i][target];
        if (all(light <= 0)) continue;

        unshadowed += light;
        lit += light * LightVisibility(i, surface, bias, near, Hash(seed + i));
    }
    Visibility[pixel] = float4(select(unshadowed > 0, lit / max(unshadowed, 1e-20), 1.0), any(unshadowed > 0));
    Unshadowed[pixel] = float4(unshadowed, 0);
    if (!(Flags & Bounce)) return;

    uint packedNormal = Normals[pixel];
    Bounced[pixel] = packedNormal ? float4(BouncedLight(pixel, surface, DecodeNormal(packedNormal)), 1) : 0;
}
