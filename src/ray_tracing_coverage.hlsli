// Alpha testing for traced rays: a hit on an alpha-tested triangle counts only where its material's
// baked coverage is set. The including shader declares Materials, Coordinates and Coverage, and
// MaskedBase and MaterialDraws among its constants, as ray_tracing_trace.hlsl does.

static const uint AtlasSlots = 32, SlotSize = 256; // Coverage slots per atlas row, texels per slot side
static const uint FadeLevels = 16;                  // Fades baked from 0 to 1
static const uint NoMaterial = 0xffffffff;

float3 CoordinatesOf(uint vertex)
{
    return Coordinates[uint2(vertex % 4096, vertex / 4096)].xyz;
}

// Whether an alpha-tested triangle is opaque at the given barycentrics. Draws without a baked
// material are let through.
bool Covered(uint instance, uint primitive, float2 barycentrics)
{
    uint first = MaskedBase[instance] + primitive * 3, low = 0, high = MaterialDraws;
    while (high - low > 1)
    {
        uint middle = (low + high) / 2;
        if (Materials[middle].x <= first) low = middle;
        else high = middle;
    }
    uint slot = MaterialDraws ? Materials[low].y : NoMaterial;
    if (slot == NoMaterial) return false;

    float3 value = CoordinatesOf(first) * (1 - barycentrics.x - barycentrics.y) + CoordinatesOf(first + 1) * barycentrics.x
        + CoordinatesOf(first + 2) * barycentrics.y;
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
