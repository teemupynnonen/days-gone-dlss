// Ray queries through the whole captured scene: alpha testing and sun visibility. The including
// shader declares Scene, Materials, Coordinates, Coverage, CachedDraws, CacheCoordinates and
// TemplateCoordinates, and MaskedBase, MaterialDraws and Sun among its constants, as
// ray_tracing_trace.hlsl does.

static const uint AtlasSlots = 32, SlotSize = 256; // Coverage slots per atlas row, texels per slot side
static const uint FadeLevels = 16;                  // Fades baked from 0 to 1
static const uint NoMaterial = 0xffffffff;

float3 CoordinatesOf(uint vertex)
{
    return Coordinates[uint2(vertex % 4096, vertex / 4096)].xyz;
}

// Whether an alpha-tested triangle is opaque at the given barycentrics: instances 0 and 1 hold this
// frame's captures, later ones cached draws. Draws without a baked material are let through.
bool Covered(uint instance, uint primitive, float2 barycentrics)
{
    float3 weights = float3(1 - barycentrics.x - barycentrics.y, barycentrics);
    uint slot;
    float3 value;
    if (instance >= 2)
    {
        uint2 cached = CachedDraws[instance - 2];
        uint first = cached.x + primitive * 3;
        slot = cached.y & 0x7fffffff;
        if (cached.y >> 31)
            value = TemplateCoordinates[first].xyz * weights.x + TemplateCoordinates[first + 1].xyz * weights.y
                + TemplateCoordinates[first + 2].xyz * weights.z;
        else
            value = CacheCoordinates[first].xyz * weights.x + CacheCoordinates[first + 1].xyz * weights.y
                + CacheCoordinates[first + 2].xyz * weights.z;
    }
    else
    {
        uint first = MaskedBase[instance] + primitive * 3, low = 0, high = MaterialDraws;
        while (high - low > 1)
        {
            uint middle = (low + high) / 2;
            if (Materials[middle].x <= first) low = middle;
            else high = middle;
        }
        slot = MaterialDraws ? Materials[low].y : NoMaterial;
        value = CoordinatesOf(first) * weights.x + CoordinatesOf(first + 1) * weights.y + CoordinatesOf(first + 2) * weights.z;
    }
    if (slot == NoMaterial) return false;

    uint2 texel = min(uint2(frac(value.xy) * SlotSize), SlotSize - 1);
    // A fade passes from halfway between the level baked and the one below. Materials without a fade
    // bake only level 0 or never, which any value passes or fails alike; theirs is not streamed.
    float level = Coverage[uint2(slot % AtlasSlots, slot / AtlasSlots) * SlotSize + texel] * 255 / 16;
    return saturate(value.z) * (FadeLevels - 1) + 0.5 >= level;
}

// Runs a query to its end, accepting alpha-tested candidates only where they are covered.
template<uint Flags> void Resolve(inout RayQuery<Flags> query)
{
    while (query.Proceed())
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && Covered(query.CandidateInstanceIndex(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
}

// Whether captured geometry of the given instances lies between `near` and `far` towards the sun.
bool Occluded(float3 start, uint mask, float near, float far)
{
    RayDesc ray;
    ray.Origin = start;
    ray.Direction = Sun.xyz;
    ray.TMin = near;
    ray.TMax = far;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
    Resolve(query);
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// 1 where no captured geometry of the given instances lies between the point and the sun. The
// instanced foliage (mask 4) repeats the trees on screen, wind and level of detail aside, so it only
// occludes from `near` on: its copy of a tree never shadows that tree's own leaves.
float TraceSun(float3 start, uint mask, float near)
{
    if (Occluded(start, mask & 3, 0, 1e30)) return 0;
    return (mask & 4) && Occluded(start, 4, near, 1e30) ? 0 : 1;
}
