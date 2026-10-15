// Traces ambient occlusion in the form of Bend's screen-space ambient occlusion (cs_6_5, compiled by
// dxc at build time). Bend stores the visibility toward the eight corners of a cube around each
// pixel, 6 bits each in an R32G32_UINT target: corner (-1,-1,1), its opposite, (1,-1,1), its opposite,
// (-1,1,1), its opposite, (1,1,1), its opposite. Of each opposite pair, the lighting reads the corner
// on the normal's side, so only those four are traced. Each gets one cosine-weighted ray from the
// game's surface into the part of the hemisphere nearest its corner, and occluders within a radius
// count more the nearer they are. The top bytes hold Bend's depth; Direct3D 11 fills them in.
#define AmbientRootSignature "RootConstants(num32BitConstants=23, b0), SRV(t0), SRV(t2), SRV(t7), SRV(t8), SRV(t9), " \
    "DescriptorTable(SRV(t3, numDescriptors=4), UAV(u0))"

cbuffer AmbientParameters : register(b0)
{
    row_major float4x4 WorldFromClip;
    uint2 Size;
    uint2 MaskedBase;     // Per instance: its first alpha-tested vertex in Coordinates
    uint MaterialDraws;   // Entries in Materials
    uint Frame;           // Varies the rays from frame to frame
    uint HalfRays;        // Each pixel traces two corners and takes the other two from its neighbour
};
RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<uint2> Materials : register(t2); // Per alpha-tested draw: first vertex, coverage slot
Texture2D<float> GameDepth : register(t3);        // The game's device depth over the view, 0 for sky
Texture2D<float4> Coordinates : register(t4);     // Alpha-tested vertices' texture coordinates and fade, 4096 a row
Texture2D<float> Coverage : register(t5);         // Each material's lowest passing fade level * 16 / 255; 1: never
Texture2D<uint> Normals : register(t6);           // The game's G-buffer normal over the view, 0 for sky
RWTexture2D<uint4> Output : register(u0);         // xy: visibility of the four traced corners, 6 bits each
StructuredBuffer<uint2> CachedDraws : register(t7);          // Per cached instance (from 2): first coordinate, coverage slot
StructuredBuffer<float4> CacheCoordinates : register(t8);    // Cached alpha-tested vertices' texture coordinates and fade
StructuredBuffer<float4> TemplateCoordinates : register(t9); // Those of the foliage templates (slots with the top bit set)

#include "ray_tracing_scene.hlsli"

static const float3 Corners[4] = { float3(-1, -1, 1), float3(1, -1, 1), float3(-1, 1, 1), float3(1, 1, 1) };
// Occluders count within 1 m plus 2.5% of the view distance: the game's screen-space occlusion also
// reaches further at a distance. That covers the ground under a car or a porch; farther ones would
// take away light the game's ambient lighting already gets from the surroundings, such as the
// ground below a wall.
static const float Radius = 100, RadiusPerDistance = 0.025;
// The trees' copies count from 30 cm plus 0.2% of the view distance on, as for sun rays.
static const float CopyDistance = 30, CopyDistancePerDistance = 2e-3;

float3 Unproject(float2 screen, float depth)
{
    float4 world = mul(float4(screen, depth, 1), WorldFromClip);
    return world.xyz / world.w;
}

// Bend's octahedral normal, 10 bits a component in bits 0 to 19 (BuildAmbientMaskCS, the lighting).
float3 DecodeNormal(uint packed)
{
    float2 encoded = float2(packed & 1023, packed >> 10 & 1023) * (2.0 / 1023) - 1;
    float3 normal = float3(encoded, 1 - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0) normal.xy = (1 - abs(normal.yx)) * select(normal.xy >= 0, 1.0, -1.0);
    return normalize(normal);
}

uint Hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 Random(inout uint seed)
{
    uint a = Hash(seed), b = Hash(a);
    seed = b;
    return float2(a, b) / 4294967296.0;
}

float3 CosineDirection(float3 normal, float2 random)
{
    // An orthonormal basis without branches (Duff et al. 2017).
    float flip = normal.z >= 0 ? 1 : -1;
    float a = -1 / (flip + normal.z), b = normal.x * normal.y * a;
    float3 tangent = float3(1 + flip * normal.x * normal.x * a, flip * b, -flip * normal.x);
    float3 bitangent = float3(b, flip + normal.y * normal.y * a, -normal.y);
    float radius = sqrt(random.x), angle = 6.2831853 * random.y;
    return (tangent * cos(angle) + bitangent * sin(angle)) * radius + normal * sqrt(1 - random.x);
}

// 1 where nothing lies within the radius, else the squared fraction of it the ray travels: an
// occluder at half the radius takes away three quarters of the light, and the edge of the radius
// does not show. The fraction itself would leave the ground under a car most of its light.
// The view's geometry (instance mask 1, with the cached draws standing in for it) occludes from the
// start. The trees' copies (mask 4) stand in for most of the view's foliage but keep the wind of the
// frame their template came from, so they occlude from `copyDistance` on: a leaf is not occluded by
// its own copy. The shadow casters are the sun shadow map's coarser level of detail, placed from the
// map, and stand off the surfaces they double by more than these short rays allow.
float Visibility(float3 start, float3 direction, float radius, float copyDistance)
{
    RayDesc ray;
    ray.Origin = start;
    ray.Direction = direction;
    ray.TMin = 0;
    ray.TMax = radius;
    RayQuery<RAY_FLAG_NONE> view;
    view.TraceRayInline(Scene, RAY_FLAG_NONE, 1, ray);
    Resolve(view);
    if (view.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ray.TMax = view.CommittedRayT();
    if (copyDistance < ray.TMax)
    {
        ray.TMin = copyDistance;
        RayQuery<RAY_FLAG_NONE> copies;
        copies.TraceRayInline(Scene, RAY_FLAG_NONE, 4, ray);
        Resolve(copies);
        if (copies.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ray.TMax = copies.CommittedRayT();
    }
    float travelled = ray.TMax / radius;
    return travelled * travelled;
}

// A corner's visibility in 6 bits, from one cosine-weighted ray into the part of the hemisphere
// nearer that corner than the other three, drawn by rejection: each takes about a quarter of it. The
// corner itself if none is drawn.
uint CornerVisibility(float3 sides[4], uint pair, float3 normal, float3 start, float radius, float copyDistance, inout uint seed)
{
    float3 direction = normalize(sides[pair]);
    for (uint attempt = 0; attempt < 16; ++attempt)
    {
        float3 candidate = CosineDirection(normal, Random(seed));
        float nearness = dot(candidate, sides[pair]);
        if (all(float4(dot(candidate, sides[0]), dot(candidate, sides[1]), dot(candidate, sides[2]), dot(candidate, sides[3])) <= nearness))
        {
            direction = candidate;
            break;
        }
    }
    return uint(Visibility(start, direction, radius, copyDistance) * 63 + 0.5);
}

// With HalfRays each pixel traces two of the four corners, the other two in a checkerboard that
// changes every frame. It takes those two from its horizontal or else its vertical partner in the
// group, the first that sees the same corners at about the same distance. Without one, its own two
// stand in for them: tracing them instead would cost a group of threads all four rays wherever one
// pixel needs them. A pixel shares its corners' visibilities (6 bits each), which corners are on its
// normal's side (bits 24 to 27) and whether it has a surface (bit 28).
groupshared uint SharedCorners[8][8];
groupshared float SharedDistance[8][8];

[RootSignature(AmbientRootSignature)]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID, uint2 local : SV_GroupThreadID)
{
    // Every thread reaches the barrier below, so none returns before it.
    bool inside = all(pixel < Size);
    float depth = inside ? GameDepth[pixel] : 0;
    uint packedNormal = inside ? Normals[pixel] : 0;
    bool surface = depth > 0 && packedNormal;
    float3 normal = DecodeNormal(packedNormal), sides[4];
    uint sideBits = 0;
    for (uint pair = 0; pair < 4; ++pair)
    {
        sides[pair] = dot(normal, Corners[pair]) > 0 ? Corners[pair] : -Corners[pair];
        if (all(sides[pair] == Corners[pair])) sideBits |= 1u << pair;
    }

    uint corners[4] = { 0, 0, 0, 0 };
    uint traced = !HalfRays ? 15 : (pixel.x ^ pixel.y ^ Frame) & 1 ? 12 : 3;
    float viewDistance = 0, radius = 0, copyDistance = 0;
    float3 start = 0;
    uint seed = Hash(pixel.x + Hash(pixel.y + Hash(Frame)));
    if (surface)
    {
        float2 screen = (pixel + 0.5) / Size * float2(2, -2) + float2(-1, 1);
        float3 eye = Unproject(screen, 1), position = Unproject(screen, depth);
        viewDistance = length(position - eye);
        // Float precision falls with distance, so the start moves further off the surface. Moving toward
        // the eye as well keeps it in front of the surface where the G-buffer's normal tilts into it.
        float bias = 0.5 + viewDistance * 1e-3;
        start = position + (normal + (eye - position) / viewDistance) * bias;
        radius = Radius + RadiusPerDistance * viewDistance;
        copyDistance = CopyDistance + CopyDistancePerDistance * viewDistance;
        for (uint pair = 0; pair < 4; ++pair)
            if (traced >> pair & 1) corners[pair] = CornerVisibility(sides, pair, normal, start, radius, copyDistance, seed);
    }
    if (HalfRays)
    {
        uint own = surface ? 1u << 28 | sideBits << 24 : 0;
        for (uint pair = 0; pair < 4; ++pair) own |= corners[pair] << (pair * 6);
        SharedCorners[local.y][local.x] = own;
        SharedDistance[local.y][local.x] = viewDistance;
        GroupMemoryBarrierWithGroupSync();

        uint ownSum = 0;
        for (uint pair = 0; pair < 4; ++pair) ownSum += corners[pair];
        uint source = (ownSum + 1) / 2 * 0x41041; // The own two's mean in every corner
        for (uint side = 0; side < 2; ++side)
        {
            uint2 at = side ? uint2(local.x, local.y ^ 1) : uint2(local.x ^ 1, local.y);
            uint partner = SharedCorners[at.y][at.x];
            if ((partner >> 28 & 1) && (partner >> 24 & 15) == sideBits && abs(SharedDistance[at.y][at.x] - viewDistance) <= 0.02 * viewDistance)
            {
                source = partner;
                break;
            }
        }
        for (uint pair = 0; pair < 4; ++pair)
            if (!(traced >> pair & 1)) corners[pair] = source >> (pair * 6) & 63;
    }
    if (!inside) return;

    uint2 packed = 0;
    if (surface)
        for (uint pair = 0; pair < 4; ++pair)
        {
            uint slot = pair * 2 + (sideBits >> pair & 1 ? 0 : 1);
            if (slot < 4) packed.x |= corners[pair] << (slot * 6);
            else packed.y |= corners[pair] << (slot * 6 - 24);
        }
    Output[pixel] = uint4(packed, 0, 0);
}
