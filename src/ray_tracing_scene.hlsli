// Ray queries through the whole captured scene (alpha testing and occlusion) and Bend's G-buffer
// normal. The including shader declares Scene, Coordinates, Coverage, CachedDraws,
// CacheCoordinates and TemplateCoordinates, and MaskedBase among its constants, as
// ray_tracing_trace.hlsl does.

static const uint AtlasSlots = 32, SlotSize = 256; // Coverage slots per atlas row, texels per slot side
static const uint FadeLevels = 16;                  // Fades baked from 0 to 1
static const uint NoMaterial = 0xffffffff;

float4 CoordinatesOf(uint vertex)
{
    return Coordinates[uint2(vertex % 4096, vertex / 4096)];
}

// Whether an alpha-tested triangle is opaque at the given barycentrics: instances 0 and 1 hold this
// frame's captures, whose vertices carry their draw's coverage slot in w (a float, -1 without one),
// later ones cached draws, whose slot comes with the instance. Either way the candidate costs no
// search: this runs for every alpha-tested candidate of every ray. Draws without a baked material are
// let through.
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
        uint first = MaskedBase[instance] + primitive * 3;
        float4 a = CoordinatesOf(first);
        slot = a.w < 0 ? NoMaterial : uint(a.w);
        value = a.xyz * weights.x + CoordinatesOf(first + 1).xyz * weights.y + CoordinatesOf(first + 2).xyz * weights.z;
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

// Whether captured geometry of the given instances lies between `near` and `far` along `direction`.
bool Occluded(float3 start, float3 direction, uint mask, float near, float far)
{
    RayDesc ray;
    ray.Origin = start;
    ray.Direction = direction;
    ray.TMin = near;
    ray.TMax = far;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
    Resolve(query);
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

uint Hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Two numbers in [0, 1) from a seed.
float2 RandomPair(uint seed)
{
    uint a = Hash(seed), b = Hash(a);
    return float2(a, b) / 4294967296.0;
}

// An orthonormal basis around a unit vector, without branches (Duff et al. 2017).
void Basis(float3 normal, out float3 tangent, out float3 bitangent)
{
    float flip = normal.z >= 0 ? 1 : -1;
    float c = -1 / (flip + normal.z), d = normal.x * normal.y * c;
    tangent = float3(1 + flip * normal.x * normal.x * c, flip * d, -flip * normal.x);
    bitangent = float3(d, flip + normal.y * normal.y * c, -normal.y);
}

// Bend's octahedral normal, 10 bits a component in bits 0 to 19 (BuildAmbientMaskCS, the lighting).
float3 DecodeNormal(uint packed)
{
    float2 encoded = float2(packed & 1023, packed >> 10 & 1023) * (2.0 / 1023) - 1;
    float3 normal = float3(encoded, 1 - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0) normal.xy = (1 - abs(normal.yx)) * select(normal.xy >= 0, 1.0, -1.0);
    return normalize(normal);
}

// Bend lights leaves and grass with lighting model 2, which the game's normals carry in bits 28 to 31
// (GameNormals); the ground and most other surfaces are model 1.
static const uint FoliageModel = 2;

// Where rays from a pixel start seeing the trees' copies. Foliage sways with the wind, but its copies
// keep the pose of their template's frame, so rays from foliage see them only from 30 cm plus 0.2% of
// the view distance on: a leaf is not shadowed by its own copy. Rays from other surfaces see them from
// the start, or leaf litter and grass would shadow nothing within that distance of the ground under
// them.
float CopiesFrom(uint packedNormal, float viewDistance)
{
    return packedNormal >> 28 == FoliageModel ? 30 + viewDistance * 2e-3 : 0;
}
