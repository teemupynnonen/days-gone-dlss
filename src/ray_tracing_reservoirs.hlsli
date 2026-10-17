// Reuse of the bounce rays' samples over frames and between neighbouring pixels (ReSTIR GI, Ouyang
// et al. 2021). A sample is the point a pixel's bounce ray found, with its normal and the light it
// sends back. Each pixel keeps a reservoir: one sample chosen from many, each with a probability in
// proportion to the light it brings the pixel, and a weight that keeps the estimate right. In a dark
// room lit through a small patch (the flashlight's spot, a moonlit window) most rays find nothing;
// once one pixel's ray finds the patch, its neighbours and the next frames can choose it too.
//
// The including shader declares GameDepth, Normals, ClipFromWorld, Size and Frame, includes
// ray_tracing_scene.hlsli, and defines before this file:
// - CopyMask, CopyDistance and CopyDistancePerDistance: the instances its bounce rays see as copies of
//   the view's geometry, from that distance on;
// - Unproject(screen, depth): the translated world position of a pixel's depth;
// - LightFrom(position, normal): the light a point sends back, as this frame lights it.

// A reservoir carries at most this many frames of its pixel's history, so a sample chosen long ago
// gives way to new ones.
static const float MaxHistory = 20;
// Relit by this frame's light, last frame's sample counts for at most this many times what it did:
// one chosen while barely lit (at the edge of a spot's cone) keeps a weight made for that, and would
// otherwise bring far more light than any sample around it.
static const float MaxRelitGrowth = 10;
// Each pixel takes the reservoirs of this many neighbours within this share of the view's height,
// which covers the same part of the scene at any render scale.
static const uint Neighbours = 4;
static const float NeighbourRadius = 0.025;

struct PackedReservoir
{
    float3 position; // The chosen sample's point, in the translated world of the frame that wrote it
    uint normal;     // Its normal, toward the pixel that found it: octahedral, 16 bits a component
    uint2 light;     // Halves: the light it sends back (rgb), and the view depth of the pixel's surface in metres
    float weight;    // Its weight in the estimate (W)
    uint surface;    // The pixel's G-buffer normal (Bend's 20 bits), and above them the samples the reservoir stands for
};

cbuffer ReuseParameters : register(b1)
{
    row_major float4x4 PreviousClipFromWorld; // This frame's translated world to last frame's clip
    float3 CameraMovement; // Last frame's translated world is this frame's moved by it
    uint Reuse;            // The samples go through the reservoirs; otherwise each pixel's own ray lights it
    uint History;          // Last frame's reservoirs hold this view's surfaces
    uint Current;          // The half of Reservoirs this frame writes
};
RWStructuredBuffer<PackedReservoir> Reservoirs : register(u3); // This frame's half and last frame's
Texture2D<float> BlueNoise : register(t30);                    // 64 by 64, tiled over the view

struct Sample
{
    float3 position; // In this frame's translated world
    float3 normal;   // Toward the pixel that found it
    float3 light;    // The light it sends back
};

struct Reservoir
{
    Sample chosen;
    float weightSum; // Of every candidate streamed in
    float count;     // The samples it stands for (M)
    float weight;    // The chosen sample's weight in the estimate (W)
};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Two numbers in [0, 1) for a pixel's bounce direction: blue noise, moved along the R2 sequence
// (Roberts 2018) every frame. Neighbouring pixels take evenly spread directions, which the filters
// smooth far better than white noise's clumps, and a pixel's directions do not repeat for a long time.
// The second number reads the texture half its size away.
float2 BlueNoisePair(uint2 pixel)
{
    float2 noise = float2(BlueNoise[pixel % 64], BlueNoise[(pixel + 32) % 64]);
    // R2's steps in 32-bit fixed point, exact for any frame number.
    return frac(noise + float2(Frame * uint2(3242174889u, 2447445414u)) / 4294967296.0);
}

uint EncodeDirection(float3 direction)
{
    float2 folded = direction.xy / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    if (direction.z < 0) folded = (1 - abs(folded.yx)) * select(folded >= 0, 1.0, -1.0);
    uint2 bits = uint2(round(saturate(folded * 0.5 + 0.5) * 65535));
    return bits.x | bits.y << 16;
}

float3 DecodeDirection(uint packed)
{
    float2 folded = float2(packed & 65535, packed >> 16) / 65535 * 2 - 1;
    float3 direction = float3(folded, 1 - abs(folded.x) - abs(folded.y));
    if (direction.z < 0) direction.xy = (1 - abs(direction.yx)) * select(direction.xy >= 0, 1.0, -1.0);
    return normalize(direction);
}

float2 ScreenOf(uint2 pixel)
{
    return (pixel + 0.5) / Size * float2(2, -2) + float2(-1, 1);
}

// The game's surface at a pixel, where it drew one: its translated world position and normal.
bool SurfaceAt(uint2 pixel, out float3 surface, out float3 normal, out uint packedNormal)
{
    float depth = GameDepth[pixel];
    packedNormal = Normals[pixel];
    surface = Unproject(ScreenOf(pixel), depth);
    normal = DecodeNormal(packedNormal);
    return depth > 0 && packedNormal;
}

// Where a pixel's bounce rays start. The translated world has the camera at its origin. Float
// precision falls with distance, so the start moves further off the surface; moving toward the eye
// as well keeps it in front of the surface where the G-buffer's normal tilts into it.
float3 RayOrigin(float3 surface, float3 normal, float viewDistance)
{
    return surface + (normal - surface / viewDistance) * (0.5 + viewDistance * 1e-3);
}

// Whether nothing captured lies between a pixel's surface and a point, as its own bounce ray would
// have looked: the view's geometry, and the copies from CopyDistance on.
bool Reaches(float3 surface, float3 normal, float viewDistance, float3 target)
{
    float3 origin = RayOrigin(surface, normal, viewDistance), toTarget = target - origin;
    float distance = length(toTarget);
    float3 direction = toTarget / distance;
    float far = distance - (0.5 + length(target) * 1e-3), near = CopyDistance + CopyDistancePerDistance * viewDistance;
    return far <= 0 || !(Occluded(origin, direction, 1, 0, far) || (near < far && Occluded(origin, direction, CopyMask, near, far)));
}

// `part`: 0 or 1, the half of Reservoirs.
uint ReservoirIndex(uint part, uint2 pixel)
{
    return (part * Size.y + pixel.y) * Size.x + pixel.x;
}

void Store(uint2 pixel, Reservoir r, float viewDepth, uint packedNormal)
{
    float3 light = min(r.chosen.light, 65504);
    PackedReservoir packed;
    packed.position = r.chosen.position;
    packed.normal = EncodeDirection(r.chosen.normal);
    packed.light = uint2(f32tof16(light.r) | f32tof16(light.g) << 16, f32tof16(light.b) | f32tof16(viewDepth / 100) << 16);
    packed.weight = r.weight;
    packed.surface = (packedNormal & 0xfffff) | uint(min(r.count, 4095)) << 20;
    Reservoirs[ReservoirIndex(Current, pixel)] = packed;
}

// Pixels without a surface keep an empty reservoir, which nothing reuses.
void Clear(uint2 pixel)
{
    Store(pixel, (Reservoir)0, 0, 0);
}

Reservoir Load(uint part, uint2 pixel, out float viewDepth, out uint packedNormal)
{
    PackedReservoir packed = Reservoirs[ReservoirIndex(part, pixel)];
    Reservoir r;
    r.chosen.position = packed.position;
    r.chosen.normal = DecodeDirection(packed.normal);
    r.chosen.light = float3(f16tof32(packed.light.x), f16tof32(packed.light.x >> 16), f16tof32(packed.light.y));
    r.weightSum = 0;
    r.count = packed.surface >> 20;
    r.weight = packed.weight;
    viewDepth = f16tof32(packed.light.y >> 16) * 100;
    packedNormal = packed.surface & 0xfffff;
    return r;
}

// What the samples are chosen by: the light a sample brings the surface, before its diffuse colour.
// None where the point lies behind the surface or shows it its back.
float Target(Sample s, float3 surface, float3 normal)
{
    float3 toPoint = normalize(s.position - surface);
    return dot(s.normal, toPoint) < 0 ? Luminance(s.light) * saturate(dot(normal, toPoint)) : 0;
}

// Streams a candidate into the reservoir, which keeps it in place of its sample with probability
// weight over the weights so far. Returns whether it did.
bool Add(inout Reservoir r, Sample candidate, float weight, float count, inout uint seed)
{
    r.weightSum += weight;
    r.count += count;
    seed = Hash(seed);
    if (!(weight > 0) || seed / 4294967296.0 * r.weightSum >= weight) return false;

    r.chosen = candidate;
    return true;
}

// A sample found from one surface point and used for another is seen at another distance and angle:
// the ratio of the solid angles it covers from the two (ReSTIR GI's reconnection Jacobian).
float Jacobian(Sample s, float3 from, float3 to)
{
    float3 toFrom = from - s.position, toTo = to - s.position;
    float cosFrom = dot(s.normal, normalize(toFrom)), cosTo = dot(s.normal, normalize(toTo));
    return cosFrom > 0 && cosTo > 0 ? cosTo / cosFrom * dot(toFrom, toFrom) / dot(toTo, toTo) : 0;
}

// Stores the pixel's reservoir for this frame: its new sample, from a cosine-weighted direction, and
// the reservoir its surface had last frame, its sample relit by this frame's light so a moving light
// leaves no trail.
void StoreChoice(uint2 pixel, float3 surface, float3 normal, uint packedNormal, Sample found)
{
    uint seed = Hash(pixel.x + Hash(pixel.y + Hash(Frame * 3 + 1)));
    Reservoir r = (Reservoir)0;
    // The target over the direction's density, cos / pi.
    Add(r, found, 3.14159265 * Luminance(found.light), 1, seed);

    float4 clip = mul(float4(surface, 1), PreviousClipFromWorld);
    int2 texel = int2(floor((clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * Size));
    if (History && clip.w > 0 && all(texel >= 0) && all(texel < int2(Size)))
    {
        float viewDepth;
        uint previousNormal;
        Reservoir previous = Load(1 - Current, texel, viewDepth, previousNormal);
        // The same surface: seen at the depth this one reprojects to, facing the same way. A reservoir
        // whose rays found nothing has no sample to relight, only its count.
        if (previous.count > 0 && abs(viewDepth / clip.w - 1) < 0.05 && dot(DecodeNormal(previousNormal), normal) > 0.9)
        {
            float count = min(previous.count, MaxHistory), weight = 0;
            if (previous.weight > 0)
            {
                previous.chosen.position -= CameraMovement;
                float before = Target(previous.chosen, surface, normal);
                previous.chosen.light = LightFrom(previous.chosen.position, previous.chosen.normal);
                weight = min(Target(previous.chosen, surface, normal), MaxRelitGrowth * before) * previous.weight * count;
            }
            Add(r, previous.chosen, weight, count, seed);
        }
    }

    float target = Target(r.chosen, surface, normal);
    r.weight = target > 0 ? r.weightSum / (r.count * target) : 0;
    Store(pixel, r, mul(float4(surface, 1), ClipFromWorld).w, packedNormal);
}

// The light bounced toward the pixel's surface, over its diffuse colour: its reservoir combined with
// those of a few neighbours on the same surface, whose samples it sees from its own point. A
// neighbour's chosen point may be hidden from it, which a ray checks.
float3 ReusedLight(uint2 pixel, float3 surface, float3 normal, float viewDistance)
{
    uint seed = Hash(pixel.x + Hash(pixel.y + Hash(Frame * 3 + 2)));
    float viewDepth;
    uint packedNormal;
    Reservoir own = Load(Current, pixel, viewDepth, packedNormal), r = (Reservoir)0;
    Add(r, own.chosen, Target(own.chosen, surface, normal) * own.weight * own.count, own.count, seed);

    bool neighbourChosen = false;
    float radius = Size.y * NeighbourRadius, tolerance = 10 + 0.01 * viewDistance;
    for (uint i = 0; i < Neighbours; ++i)
    {
        float2 random = RandomPair(seed + i);
        float angle = 6.2831853 * random.y, distance = (0.1 + 0.9 * sqrt(random.x)) * radius;
        int2 texel = int2(pixel) + int2(round(float2(cos(angle), sin(angle)) * distance));
        if (any(texel < 0) || any(texel >= int2(Size))) continue;

        float3 neighbourSurface, neighbourNormal;
        uint packed;
        // On the same surface: near the pixel's plane and facing the same way.
        if (!SurfaceAt(texel, neighbourSurface, neighbourNormal, packed) || abs(dot(normal, neighbourSurface - surface)) > tolerance
            || dot(neighbourNormal, normal) < 0.9)
            continue;

        Reservoir neighbour = Load(Current, texel, viewDepth, packed);
        float jacobian = Jacobian(neighbour.chosen, neighbourSurface, surface);
        if (neighbour.count == 0 || !(jacobian > 0.1 && jacobian < 10)) continue;

        float weight = Target(neighbour.chosen, surface, normal) * neighbour.weight * neighbour.count * jacobian;
        neighbourChosen = Add(r, neighbour.chosen, weight, neighbour.count, seed) || neighbourChosen;
    }

    float target = Target(r.chosen, surface, normal);
    if (target <= 0 || (neighbourChosen && !Reaches(surface, normal, viewDistance, r.chosen.position))) return 0;

    // The diffuse surface's light, cos / pi over the colour, times the sample's weight. The output is
    // half precision: beyond its range a value turns infinite, and the filter would spread it.
    float3 toPoint = normalize(r.chosen.position - surface);
    float3 light = r.chosen.light * saturate(dot(normal, toPoint)) / 3.14159265 * r.weightSum / (r.count * target);
    return all(isfinite(light)) ? min(light, 65504) : 0;
}
