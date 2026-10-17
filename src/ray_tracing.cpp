#include "ray_tracing.hpp"
#include "alpha_test.hpp"
#include "pass_timer.hpp"
#include "ray_tracing_shaders.hpp"
#include "ray_tracing_trace.h" // TraceShader: ray_tracing_trace.hlsl compiled by dxc at build time
#include "ray_tracing_ao.h"    // AmbientTraceShader: ray_tracing_ao.hlsl, likewise
#include "ray_tracing_bounce.h" // BounceTraceShader: ray_tracing_bounce.hlsl, likewise
#include "ray_tracing_lights.h" // LightsTraceShader: ray_tracing_lights.hlsl, likewise
#include "shader_bytecode.hpp"
#include "upscaler.hpp"

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace days_gone_dlss::ray_tracing
{
namespace
{
using Microsoft::WRL::ComPtr;

// Captured vertices, three per triangle, travel to D3D12 in a shared texture of 4096 a row. Opaque
// draws fill it from the start; alpha-tested ones, captured from the depth prepass with their
// texture coordinates, end at MaskedEnd. Each range starts on a triangle. The vertices take the same
// room again in the stream-output buffer and the D3D12 vertex buffer (Direct3D 11 cannot share a
// buffer with D3D12, and stream output cannot write a texture), so the room, 4.2 million triangles,
// just covers the worst view seen: 3.9 million in the Hot Springs forest with the caches off.
constexpr UINT RowLength = 4096, Rows = 3072, Capacity = RowLength * Rows, MaskedEnd = Capacity / 3 * 3;
constexpr UINT MaskedCapacity = RowLength * 1536; // Alpha-tested vertices; their coordinates use the same rows
constexpr UINT VertexBytes = 16;       // float4: clip position when streamed, world position when shared
constexpr UINT MaskedVertexBytes = 28; // float4 clip position, float2 texture coordinates, float fade
// Baked coverage of alpha-tested materials: 32 by 32 slots of 256 texels square. A forest view
// holds a few hundred: leaf atlases per species and impostors of distant trees.
constexpr UINT SlotSize = 256, AtlasSlots = 32, MaterialSlots = AtlasSlots * AtlasSlots, NoMaterial = 0xffffffff;
// A material holds the game's shader and textures. One not drawn for MaterialMemory traced frames
// (about a minute at 30 fps) lets them go and frees its slot, unless a cached draw still uses the
// slot. The stream output kept per shader pair goes likewise.
constexpr UINT MaterialMemory = 1800;
// Needles a texel or two wide in the game's textures fall between a slot's texels, and light leaks
// through the canopy. Each slot texel is baked from BakeSamples squared samples (as BakeCopy reads them).
constexpr UINT BakeSamples = 4;
// A material whose alpha test also reads a fade is baked at evenly spaced fades from 0 to 1. Each
// texel keeps the lowest level that passes, as level * 16 / 255 (255 where none does).
constexpr UINT FadeLevels = 16;
constexpr UINT MaxMaterialDraws = 8192;
constexpr UINT FramesInFlight = 3, TimestampsPerFrame = 8;
// The stream-output geometry shader made for each vertex shader, stored on the vertex shader.
constexpr GUID StreamOutputShader{0x3e8f2a61, 0x9c4d, 0x4b17, {0xa5, 0x2e, 0x71, 0xd0, 0x8b, 0x6c, 0x43, 0xf9}};
// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
// The constant buffer slot of a sun shadow resolve's light-space rows, stored on the pixel shader.
constexpr GUID ShadowResolveShader{0x5b0e7c93, 0x2d41, 0x4f8a, {0x9e, 0x16, 0xc8, 0x3a, 0x47, 0xd2, 0x05, 0xbb}};
// Marks pixel shaders that discard pixels: alpha-tested materials such as foliage cards.
constexpr GUID AlphaTestedShader{0x9d4a2e17, 0x6c38, 0x4b05, {0xb1, 0x7f, 0x2a, 0xe6, 0x50, 0x93, 0xc4, 0x8d}};
// The constant buffer slots a vertex shader declares, kept on it as a mask: other slots can hold
// buffers bound for earlier draws.
constexpr GUID ConstantSlots{0x58d1c6e2, 0x7a43, 0x4f0b, {0x9b, 0x2e, 0x14, 0xc7, 0x63, 0xa8, 0x0d, 0x95}};
// A vertex shader's bytecode, kept on it for streaming the coordinates of alpha-tested draws.
constexpr GUID VertexBytecode{0x41f7c2d8, 0x0b6e, 0x4a93, {0x8c, 0x15, 0x7e, 0xd2, 0x39, 0xa4, 0x60, 0x1f}};
// On an alpha-tested pixel shader: the vertex shader that runs it over its texture coordinates
// (alpha_test.hpp), and which of its inputs those coordinates are.
// The vertex shader is compiled from BakeShaderSource the first time the material is baked.
constexpr GUID BakeShader{0x6a3e9f04, 0x5d21, 0x4c7b, {0x93, 0x0e, 0xb4, 0x18, 0x6f, 0xc2, 0x7a, 0x55}};
constexpr GUID BakeShaderSource{0x7e19d4b3, 0x2a6f, 0x4185, {0xbc, 0x47, 0x0d, 0x93, 0x5a, 0x1e, 0xe6, 0x28}};
constexpr GUID BakeInputData{0x2c8b5e71, 0x9f46, 0x4e0d, {0xa7, 0x3b, 0x61, 0xf0, 0x4c, 0x9e, 0x12, 0xd8}};
struct BakeInput
{
    char semantic[32];
    UINT semanticIndex, component, textures;
    char fadeSemantic[32];
    UINT fadeSemanticIndex, fadeComponent, toggles;
    bool transformed;
    CoordinateTransform transform;
};
// Alpha-tested draws whose material tiles its texture (AlphaTest::transform) in a frame: each has its
// scale and offset copied from its constants into four rows of a buffer, and Positions takes its
// coordinates into the baked tile with them.
constexpr UINT MaxTransformDraws = 2048;
// A tiled draw's vertices in the alpha-tested range, its rows, and per value (scale x and y, offset
// x and y) whether it is read from its row (bit k) and which component (bits 8 + 2k), or else the literal.
struct TransformDraw { std::uint32_t first, end, row, sources; float literals[4]; };

// Bend resolves the sun's shadow into a screen-space target before lighting
// (FBendSceneDirectionalShadowWithScreenSpaceResolvePS, its fast path and their lighting-model
// variants). Each transforms world positions into shadow space with rows 18 to 20 of one constant
// buffer; row 20, the shadow projection's depth axis, is parallel to the sun direction.
struct ShadowResolve { std::array<std::uint8_t, 16> checksum; UINT constantSlot; };
constexpr ShadowResolve ShadowResolves[]{
    {upscaler::dxbcHash("4ed0a93784d641d6d3ca4719e7d4d12f"), 1}, {upscaler::dxbcHash("0f8efb57483b0cbc07fb7539e7bce746"), 2},
    {upscaler::dxbcHash("b2fce08449f06cb8022fbf394ae86b3e"), 1}, {upscaler::dxbcHash("40f3d77234e71c9abd01399f0faffc0b"), 2},
    {upscaler::dxbcHash("827dcf6c0290a3131bdfb1b3c9e7aec3"), 1}, {upscaler::dxbcHash("ec1f6dd10f32b01e44564288ba349398"), 2},
    {upscaler::dxbcHash("d0bfaf545efc596cfcd5cc19cb66d44e"), 1}, {upscaler::dxbcHash("eba0f81e95325122163613d50dc9d287"), 2},
    {upscaler::dxbcHash("48c30d8394ef59761718d0e340543a2d"), 1}, {upscaler::dxbcHash("92452d5ea33a1d9a27b5910adc7cd12b"), 1},
    {upscaler::dxbcHash("ce16d1de7e4c487731ad58f49fd1da0d"), 1}, {upscaler::dxbcHash("c5d6e8d2b20f573fe4fdd2801998b4a0"), 1},
};
constexpr UINT SunRow = 20;
// Rows 28 to 31 take the main view's translated world to the far shadow map's u, v and depth; row 35.z
// scales u into the atlas the far map shares with two nearer cascades. Copied for the shadow casters.
constexpr UINT LightRows = 28, LightRowCount = 8;
// Shadow casters arrive in runs by shadow map and viewport depth range.
constexpr UINT MaxCasterRuns = 64;
// Static view geometry kept after it leaves the view (GeometryCache): vertices and alpha-tested
// coordinates in rings of float4, bottom-level structures in a ring of bytes. The coordinates of the
// draws added in a frame are staged in whole rows.
constexpr UINT CacheVertexCapacity = RowLength * 2048, CacheStagingRows = 256;
constexpr UINT64 CacheStructureBytes = 512ull << 20, CacheScratchBytes = 32ull << 20;
// Objects drawn with velocity are never cached. One not drawn for MoverMemory traced frames (about a
// minute at 30 fps) is forgotten: its key is made of buffer addresses the game reuses for other objects.
constexpr UINT MoverMemory = 1800;
// Foliage templates have a cache of their own: a forest has hundreds of meshes, each up to tens of
// thousands of vertices, and they must not evict one another.
constexpr UINT TemplateVertexCapacity = RowLength * 1536;
constexpr UINT64 TemplateStructureBytes = 256ull << 20;
// In the per-instance table, marks a slot whose coordinates are in the template cache.
constexpr std::uint32_t TemplateCoordinates = 0x80000000;
constexpr UINT MaxCachedDraws = 8192, MaxNewCachedDraws = 128, MaxFoliageInstances = 65536;
constexpr UINT MaxInstances = 2 + MaxCachedDraws + MaxFoliageInstances;
// Foliage: instance transforms of 48 bytes, the visible lists read back every FoliageLatency-th frame,
// each with that many frames to land, the trees within FoliageRadius (cm) of the camera traced, and
// farther ones for FoliageMemory read-backs after they were last seen: about 20 s at 30 fps.
constexpr UINT FoliageTransformBytes = 48, FoliageLatency = 4, FoliageIndexCapacity = 1 << 18, FoliageReloadFrames = 1800;
constexpr UINT FoliageMemory = 150;
// A tree is a draw per section: trunk, branches, leaves, and both levels' while its level of detail
// crossfades. Sections past MaxTreeSections are not traced.
constexpr UINT MaxTreeSections = 8;
constexpr UINT NoInstance = 0xffffffff;
constexpr float FoliageRadius = 8000;

// Bend's screen-space ambient occlusion (BendScreenSpaceShadowingCS) runs at the end of the G-buffer
// pass. BuildAmbientMaskCS reads the G-buffer normal (t0) and picks the cube corners on its side,
// ComputeAmbientCS marches toward them in screen space into an R32G32_UINT target, and BlurAmbientCS,
// a 3x3 blur that rejects other depths, writes the target (u0) lighting reads. The lighting then
// accumulates it over frames. Its blur has no constants, so it can be run again on traced occlusion.
// The mask's <true> variant serves the half-resolution occlusion.
enum class AmbientPass : std::uint32_t { Mask, Blur };
constexpr GUID AmbientShader{0x1f6b3c0e, 0x8a27, 0x4d59, {0xb3, 0x41, 0x6e, 0x0c, 0x9d, 0x72, 0xa5, 0x18}};
struct AmbientShaderChecksum { std::array<std::uint8_t, 16> checksum; AmbientPass pass; };
constexpr AmbientShaderChecksum AmbientShaders[]{
    {upscaler::dxbcHash("421145d93a7c3dc04afeb4ef62946097"), AmbientPass::Mask}, // FBuildAmbientMaskCS<false>
    {upscaler::dxbcHash("5a64b2140fd3d78a3b57c3d2006e4371"), AmbientPass::Mask}, // FBuildAmbientMaskCS<true>
    {upscaler::dxbcHash("73260a675a394e71f15c5d62bc699101"), AmbientPass::Blur}, // FBlurAmbientCS
};

// Bend draws each local light (BendDeferredLightShaders) as a full-screen triangle per lighting model,
// added to the scene colour, scissored to the light and stencil-masked to its volume, which a draw
// with TDeferredLightVS<true> marks just before. With light shadows, rays take the place of the game's
// shadow maps for up to MaxLights lights a frame; the rest keep the game's. The light's constants
// (position and inverse radius, colour, direction, spot angles, source radius and length) are the
// first rows of constant buffer 4, or 5 for hair, whose cb4 holds its own parameters.
enum class LightShader : std::uint32_t { Volume, Light };
constexpr GUID LightShaderTag{0x3d7a91c4, 0x5e28, 0x4b60, {0x8f, 0x13, 0xa6, 0x0c, 0x72, 0xe9, 0x4b, 0xd5}};
constexpr auto LightVolumeShader = upscaler::dxbcHash("704bf50f025973c175c38a06cb9c43c7");   // TDeferredLightVS<true>
constexpr auto DeferredLightShader = upscaler::dxbcHash("aefaa9f51b2cdbbef44287091ea4a803"); // BendDeferredLightShaders VertexMain
constexpr UINT MaxLights = 16, LightConstantRows = 4;
constexpr int NoLightSlot = -1;
// The lights' descriptor table (LightsRootSignature): t5 to t29, then u0 to u2.
namespace LightDescriptor
{
constexpr UINT Depth = 0, Coordinates = 1, Coverage = 2, Table = 3, Slots = 4, Normals = Slots + MaxLights, Albedo = Normals + 1,
    Vertices = Albedo + 1, CacheVertices = Vertices + 1, TemplateVertices = CacheVertices + 1, Visibility = TemplateVertices + 1,
    Unshadowed = Visibility + 1, Bounced = Unshadowed + 1;
}
constexpr UINT LightDescriptors = LightDescriptor::Bounced + 1;

// How shaders::Composite shows the traced images.
enum class CompositeMode : std::uint32_t
{
    Geometry, DepthComparison, SunLight, ShadowComparison, AmbientOcclusion, AmbientComparison, OffscreenShadows, GlobalIllumination
};

struct PositionConstants
{
    float worldFromClip[16], worldFromClipXYW[16];
    std::uint32_t opaqueCount, maskedCount, maskedStart, casterRunCount;
    float atlasSize[2];
    std::uint32_t transformCount, materialDrawCount;
    std::uint32_t casterRuns[MaxCasterRuns][4]; // First opaque and alpha-tested vertex, shadow map
    float casterDepths[MaxCasterRuns][4];       // The run's viewport depth range
};
struct RegionConstants { std::int32_t renderOrigin[2]; std::uint32_t size[2]; };
struct CompositeConstants { std::int32_t outputOrigin[2], renderOrigin[2]; float renderScale[2]; CompositeMode mode; std::uint32_t padding; };
struct BakeConstants { std::uint32_t slot[2], padding[2]; };
struct BakeLevel { float fade, depth; std::uint32_t combination, transform; };
struct TraceConstants
{
    float worldFromClip[16], clipFromWorld[16];
    std::uint32_t size[2], maskedBase[2];
    float sun[4];
    // Per instance, the view's geometry then the shadow casters: the first vertex of each geometry, and
    // (maskedBase) the first alpha-tested vertex in the coordinates texture.
    std::uint32_t geometryBase[2][2], primaryMask, offscreenCheck, primaryRays, frame, softShadows;
};
static_assert(sizeof(TraceConstants) == 49 * 4, "TraceRootSignature takes 49 root constants");
// Instance masks: primary rays see the view's geometry, sun rays also the cached geometry, the
// instanced foliage and the casters of the game's sun shadow map. Light rays leave those casters out:
// moving objects are among them at the shadow map's level of detail, a coarser copy of the object the
// view drew, around its surface.
constexpr UINT ViewMask = 1, CasterMask = 2, FoliageMask = 4, SunCasterMask = 8;
struct AmbientConstants
{
    float worldFromClip[16];
    std::uint32_t size[2], maskedBase[2], frame, halfRays;
};
static_assert(sizeof(AmbientConstants) == 22 * 4, "AmbientRootSignature takes 22 root constants");
struct BounceConstants
{
    float worldFromClip[16], clipFromWorld[16];
    std::uint32_t size[2], maskedBase[2];
    float sun[4];
    std::uint32_t geometryBase[2][2], frame;
};
static_assert(sizeof(BounceConstants) == 45 * 4, "BounceRootSignature takes 45 root constants");
struct BounceFilterConstants
{
    float worldFromClip[16], previousClipFromWorld[16];
    std::uint32_t size[2], step, history, final, maxFrames, clamp, padding;
};
enum LightFlags : std::uint32_t { SoftLightShadows = 1, LightBounce = 2 };
struct LightConstants
{
    float worldFromClip[16], clipFromWorld[16];
    std::uint32_t size[2], maskedBase[2];
    std::int32_t origin[2];
    std::uint32_t lightCount, frame;
    float translation[3];
    std::uint32_t flags, geometryBase[4];
};
static_assert(sizeof(LightConstants) == 48 * 4, "LightsRootSignature takes 48 root constants");
struct LightTableConstants { std::int32_t rects[MaxLights][4]; };
// Bend's lighting reads the sun's colour from rows 33 and 140 of its constant buffer in slot 2 and the
// exposure from row 43, the camera's height from row 53 of slot 1. Copied for the bounced light.
constexpr std::pair<UINT, UINT> LightingRows[]{{2, 33}, {2, 43}, {2, 140}, {1, 53}};
constexpr UINT ExposureRow = 43;

// Static geometry the view drew, kept after it leaves the view: Bend draws no trees into its sun
// shadow maps, so trees behind the camera would stop shadowing what is in front of it. Each cached
// draw gets its own bottom-level structure, built once. A frame traces the cached draws of the
// objects it did not draw itself, and stands the cache in for the static draws it holds: the view
// skips capturing those, and they are traced as its own geometry. It stands in for the shadow casters
// of the objects it holds whole too. Positions are in the translated world of the frame that drew
// them: their instances move them by the camera's movement since.
class GeometryCache
{
public:
    // A draw the view captured this frame: its vertices in the frame's opaque range, or in the
    // alpha-tested range with the draw's coverage slot. Templates are only traced as foliage instances.
    // A stand-in is a draw the view left to the cache, which holds it.
    struct Draw { std::uint64_t key, object; UINT first, vertices, slot; bool masked, isTemplate{}, standIn{}; };
    // A cached template: its structure, first vertex and coordinate, coverage slot and the frame it came from.
    struct Template
    {
        D3D12_GPU_VIRTUAL_ADDRESS structure;
        std::uint32_t firstVertex, coordinates, slot;
        float preViewTranslation[3];

        bool operator==(const Template&) const = default;
    };

    GeometryCache(ID3D12Device5* device, UINT vertexCapacity, UINT64 structureBytes);
    // Adds the draws not cached yet, within the frame's budget, and picks the entries to trace: the
    // stand-ins as the view's geometry, and those whose object the view did not draw, or all of them, as
    // shadow casters. The entries of `casting`, objects whose shadow casters were left to the cache,
    // stay this frame. `preViewTranslation` places this frame.
    void plan(const std::vector<Draw>& draws, UINT maskedStart, bool all, const std::unordered_set<std::uint64_t>& casting,
        const float (&preViewTranslation)[3]);
    // Copies the added draws' vertices from the frame's vertex buffer and their coordinates from the
    // shared texture, then builds their structures.
    void record(ID3D12GraphicsCommandList4* list, ID3D12Resource* vertices, ID3D12Resource* coordinates);
    // The traced entries' instances, seen by sun rays like the shadow casters, with their first vertex
    // as InstanceID, and per instance its first coordinate and coverage slot. Returns their number.
    UINT write(D3D12_RAYTRACING_INSTANCE_DESC* instances, std::uint32_t (*table)[2]) const;
    D3D12_GPU_VIRTUAL_ADDRESS vertices() const { return vertexBuffer->GetGPUVirtualAddress(); }
    ID3D12Resource* vertexResource() const { return vertexBuffer.Get(); }
    UINT capacity() const { return vertexCapacity; }
    bool contains(std::uint64_t key) const { return byKey.contains(key); }
    // Whether every draw of the object the view last drew is cached, and none has been evicted since.
    bool holdsWhole(std::uint64_t object) const { return wholeObjects.contains(object); }
    // Evicts every entry of `objects`. Only before plan(), which drops the last frame's builds and
    // traced entries: until then they point at entries.
    void evictObjects(const std::unordered_set<std::uint64_t>& objects);
    // Marks the coverage slots its alpha-tested entries use.
    void markSlots(std::bitset<MaterialSlots>& slots) const;
    D3D12_GPU_VIRTUAL_ADDRESS coordinates() const { return coordinateBuffer->GetGPUVirtualAddress(); }
    std::optional<Template> find(std::uint64_t key) const;
    void clear();
    std::size_t size() const { return entries.size(); }
    std::size_t traced() const { return tracedEntries.size(); }
    std::pair<std::size_t, UINT64> building() const // Builds this frame and their vertices
    {
        UINT64 vertices{};
        for (const auto& build : builds) vertices += build.entry->vertices;
        return {builds.size(), vertices};
    }

private:
    enum class Ring { Vertices, Coordinates, Structures };
    struct Entry
    {
        std::uint64_t key, object;
        UINT vertices, slot;
        bool masked, isTemplate;
        UINT64 vertexOffset, coordinateOffset, structureOffset, structureSize;
        float preViewTranslation[3]; // Of the frame that drew it
    };
    struct Build { std::list<Entry>::iterator entry; UINT sourceVertex, sourceCoordinate; UINT64 scratchOffset; };
    // An entry's part of a ring; per ring, a frame's are sorted by offset.
    struct Room { UINT64 begin, end; };
    using RingRooms = std::array<std::vector<Room>, 3>;

    static Room roomOf(const Entry& entry, Ring ring);
    static const Room* firstOverlap(const std::vector<Room>& rooms, Room room);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs(const Entry& entry, D3D12_RAYTRACING_GEOMETRY_DESC& geometry) const;
    RingRooms keptRooms(const std::unordered_set<std::uint64_t>& standIns, const std::unordered_set<std::uint64_t>& casting) const;
    std::optional<UINT64> allocate(Ring ring, UINT64 size, RingRooms& kept);
    void evict(std::list<Entry>::iterator entry);

    ID3D12Device5* device;
    UINT vertexCapacity;
    UINT64 structureBytes;
    ComPtr<ID3D12Resource> vertexBuffer, coordinateBuffer, structures, scratch, staging;
    std::list<Entry> entries; // Oldest first
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> byKey;
    std::unordered_set<std::uint64_t> wholeObjects;
    UINT64 heads[3]{};
    std::vector<Build> builds;
    std::vector<std::pair<const Entry*, UINT>> tracedEntries; // With their instance masks
    float current[3]{}; // This frame's preViewTranslation
};


std::atomic<View> selected{View::Off};
std::atomic<bool> sunShadows{}, ambientOcclusion{}, halfAmbientRays{}, replaceCanopyShadows{}, offscreenGeometry{true}, globalIllumination{},
    lightShadows{}, softLightShadows{}, lightBounce{}, softSunShadows{}, bypassed{}, failed{};
std::atomic<std::uint64_t> toggledAt{};
std::mutex statusMutex;
Status published;

// Stream output depends only on the vertex shader's output signature, which many shaders share.
std::mutex streamOutputMutex;
std::map<std::string, ComPtr<ID3D11GeometryShader>, std::less<>> streamOutputShaders;

// The frame being captured, on the thread executing Direct3D work.
struct Capture
{
    bool open{}, traced{};
    float clipFromWorld[16]{}, preViewTranslation[3]{};
    bool cameraKnown{};
    std::int32_t rect[4]{}, unscaled[4]{};
    UINT opaqueVertices{}, maskedVertices{};
    unsigned basePassDraws{}, capturedDraws{}, maskedDraws{}, unbakedDraws{}, backgroundDraws{}, unknownShaderDraws{}, unsupportedDraws{},
        overCapacityDraws{}, casterDraws{}, repeatedCasterDraws{};
    unsigned movingDraws{}, anonymousDraws{}; // Captured draws not cached: moving or repeated, or not told apart
    unsigned copiedFoliageDraws{}; // Foliage draws left to the trees' copies, not captured
    unsigned standInDraws{}; // Static draws left to the geometry cache, not captured
    // Shadow casters follow the view's geometry in both ranges, in runs by shadow map (0 is the far
    // map) and viewport depth range.
    struct CasterRun { UINT opaqueStart, maskedStart, map; float minDepth, maxDepth; };
    std::vector<CasterRun> casterRuns;
    // The objects the view drew, captured or left to the cache; their shadow casters are not captured,
    // nor those of the objects the cache holds whole (`casting`).
    std::unordered_set<std::uint64_t> viewObjects, casting;
    unsigned casterDrawsInView{}, casterDrawsInCache{};
    // Per alpha-tested draw: its first vertex in the alpha-tested range and its coverage slot.
    std::vector<std::array<std::uint32_t, 2>> materialDraws;
    std::vector<TransformDraw> transformDraws;
    LONGLONG captureTicks{};
    ComPtr<ID3D11Resource> depth;
    unsigned shadowResolveDraws{};
    bool sunCopied{}, shadowPending{}, shadowCopied{}, shadowsInjected{};
    ComPtr<ID3D11Texture2D> shadow; // The game's resolved sun shadow
    DXGI_FORMAT shadowFormat{};
    std::unordered_set<std::uint64_t> maskedMeshes; // drawKey of the alpha-tested depth-only draws
    std::unordered_set<std::uint64_t> capturedKeys; // drawKey of the draws captured so far this frame
    std::unordered_set<std::uint64_t> viewDraws;    // cacheKey of the view's draws so far this frame
    std::vector<GeometryCache::Draw> cacheDraws;    // The view's static draws
    std::vector<GeometryCache::Draw> templateDraws; // Foliage templates offered this frame
    // Foliage draws and their visible instance lists, staged at `staged` in the frame's readback.
    struct FoliageDraw { std::uint64_t mesh; ID3D11Buffer* transforms; UINT staged, instances; std::uint64_t templateKey; };
    std::vector<FoliageDraw> foliageDraws;
    UINT foliageIndices{};
    bool foliagePlaced{}; // The foliage is prepared for this frame's trace, at its first draw
    ComPtr<ID3D11Resource> normals, ambient; // The game's G-buffer normal and ambient occlusion target
    bool ambientRead{}, ambientFiltered{}, ambientInjected{};
    ComPtr<ID3D11Resource> gbuffer[2]; // Bend's two packed G-buffer targets; the second is the normal
    ComPtr<ID3D11Resource> sceneColor; // The target lighting draws into
    DXGI_FORMAT sceneColorFormat{};
    bool bouncePending{}, bounceAdded{}; // The bounced light waits for lighting to finish, then is added
    // Local lights: each slot's scissor rectangle, and the target they add to. A light's draws (one per
    // lighting model) follow its volume's; `lightSlot` is the slot of the light being drawn, NoLightSlot
    // when the game draws it, and `lightModels` the stencil references it has drawn.
    std::vector<D3D11_RECT> lightRects;
    ComPtr<ID3D11Resource> lightTarget;
    DXGI_FORMAT lightTargetFormat{};
    bool lightOpen{}, lightVolumeDrawn{};
    int lightSlot{NoLightSlot};
    D3D11_RECT lightScissor{};
    std::uint32_t lightModels{};
    unsigned tracedLights{}, gameLights{};
    bool lightsPending{}; // Lights wait in their slots for every light to draw

    // Back to the defaults, keeping the containers' storage: a forest frame fills them with thousands
    // of entries, and growing them again every frame slows the thread executing Direct3D work.
    void reset()
    {
        Capture fresh;
        const auto keepStorage = [](auto& to, auto& from) { to = std::move(from); to.clear(); };
        keepStorage(fresh.casterRuns, casterRuns);
        keepStorage(fresh.viewObjects, viewObjects);
        keepStorage(fresh.casting, casting);
        keepStorage(fresh.materialDraws, materialDraws);
        keepStorage(fresh.transformDraws, transformDraws);
        keepStorage(fresh.maskedMeshes, maskedMeshes);
        keepStorage(fresh.capturedKeys, capturedKeys);
        keepStorage(fresh.viewDraws, viewDraws);
        keepStorage(fresh.cacheDraws, cacheDraws);
        keepStorage(fresh.templateDraws, templateDraws);
        keepStorage(fresh.foliageDraws, foliageDraws);
        keepStorage(fresh.lightRects, lightRects);
        *this = std::move(fresh);
    }
} capture;

// Direct3D 12's part of the newest frame it has finished, from its timestamps, in milliseconds.
struct Timings { float copy{}, cache{}, templates{}, view{}, casters{}, top{}, trace{}; };

struct EventCloser { void operator()(HANDLE event) const { CloseHandle(event); } };

// The bottom-level geometries of one frame: opaque, then alpha-tested, each only when present.
struct Geometries
{
    std::array<D3D12_RAYTRACING_GEOMETRY_DESC, 2> descs{};
    UINT count{};
    std::uint32_t base[2]{}; // First vertex of each
};

// The ambient occlusion rays and the game's packing of their result, created on first use.
struct AmbientGpu
{
    ComPtr<ID3D11ComputeShader> blur; // The game's BlurAmbientCS
    ComPtr<ID3D11ComputeShader> packShader;
    // Shared with D3D12 over the view: the traced corners.
    ComPtr<ID3D11Texture2D> traced;
    ComPtr<ID3D11ShaderResourceView> tracedView;
    UINT width{}, height{};
    // The size of the game's target: a copy of it, the traced occlusion packed in its place, and
    // that after the game's blur.
    ComPtr<ID3D11Texture2D> game, packed, filtered;
    ComPtr<ID3D11ShaderResourceView> gameView, packedView, filteredView;
    ComPtr<ID3D11UnorderedAccessView> packedAccess, filteredAccess;

    ComPtr<ID3D12Resource> traced12;
    ID3D12Resource* depth12{}; // The depth copy the descriptors point at
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    std::array<bool, FramesInFlight> recorded{};
    bool tracing{}; // The frame being traced includes ambient occlusion
    float milliseconds{};
};

// A noisy traced signal over the view, accumulated over frames and filtered (shaders::BounceAccumulate,
// BounceFilter): the sun's bounce, the lights' bounce and the lights' soft shadows each have one.
struct Denoiser
{
    // The accumulation, written to each in turn, with the depth it was last taken at; the filter's
    // passes take turns too.
    std::array<ComPtr<ID3D11Texture2D>, 2> history, filtered;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> historyViews, filteredViews;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> historyAccess, filteredAccess;
    ComPtr<ID3D11Texture2D> previousDepth;
    ComPtr<ID3D11ShaderResourceView> previousDepthView;
    UINT width{}, height{};
    UINT current{}; // The history written next
    UINT64 usedIn{}; // The frame (Gpu::frames) it last took: history carries over only from the frame before
    bool historyValid{};
    float previousClipFromWorld[16]{}, previousTranslation[3]{};
    ID3D11ShaderResourceView* output{}; // The last pass's
};

// Sunlight bounced once off the captured geometry, created on first use. Direct3D 12 traces a ray
// per pixel into `traced`; Direct3D 11 accumulates and filters those, and the result is added to the
// scene once lighting has drawn. The game's diffuse colours copied here also serve the local lights'
// bounce, and the filter their soft shadows too.
struct BounceGpu
{
    ComPtr<ID3D11ComputeShader> surfaceShader, accumulateShader, filterShader;
    ComPtr<ID3D11PixelShader> addShader;
    ComPtr<ID3D11BlendState> additive;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Buffer> lighting; // LightingRows of this frame's lighting constants
    ComPtr<ID3D11ShaderResourceView> lightingView;
    ComPtr<ID3D11ShaderResourceView> gbufferView, stencilView; // Of the game's textures
    // Shared with D3D12 over the view: the game's diffuse colours, and the traced bounce.
    ComPtr<ID3D11Texture2D> albedo, traced;
    ComPtr<ID3D11ShaderResourceView> albedoView, tracedView;
    ComPtr<ID3D11UnorderedAccessView> albedoAccess;
    UINT width{}, height{};
    ID3D11Texture2D* depth{}; // The depth copy depthView and the descriptors point at
    ComPtr<ID3D11ShaderResourceView> depthView;
    Denoiser denoiser;
    bool surfaces{}; // This frame's diffuse colours are copied and its normals known

    ComPtr<ID3D12Resource> albedo12, traced12;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    std::array<bool, FramesInFlight> recorded{};
    bool tracing{}; // The frame being traced includes the bounce
    float milliseconds{};
};

// The game's local lights shadowed by rays, created on first use. Each light the game draws goes,
// without the game's shadow, into a slot of its own the size of the scene colour, and its constants
// into `constants`. Once every light has drawn, Direct3D 12 traces toward them: per pixel the lights
// summed unshadowed, the share of them that reaches it, and with bounce light the light they bounce
// toward it. Direct3D 11 filters those with soft shadows and bounce light, and adds the result to the
// scene colour.
struct LightsGpu
{
    ComPtr<ID3D11ComputeShader> tableShader;
    ComPtr<ID3D11PixelShader> addShader;
    ComPtr<ID3D11BlendState> additive;
    ComPtr<ID3D11Buffer> constants, tableConstants;
    ComPtr<ID3D11ShaderResourceView> constantsView;
    // Bound in place of the game's shadow maps, lit whichever way they are read: shadow maps far
    // beyond any depth and baked shadow volumes at 1. Cube maps are compared by a sampler; `near`
    // passes the comparisons that pass toward greater depths.
    ComPtr<ID3D11ShaderResourceView> litMap, litCube, litCubeNear, litVolume;
    // Shared with D3D12: the slots, the light table (LightTable) and the traced images over the view.
    std::array<ComPtr<ID3D11Texture2D>, MaxLights> slots;
    std::array<ComPtr<ID3D11RenderTargetView>, MaxLights> slotTargets;
    std::array<ComPtr<ID3D12Resource>, MaxLights> slots12;
    ComPtr<ID3D11Texture2D> table, visibility, unshadowed, bounced;
    ComPtr<ID3D11UnorderedAccessView> tableAccess;
    ComPtr<ID3D11ShaderResourceView> visibilityView, unshadowedView, bouncedView;
    UINT width{}, height{};
    ComPtr<ID3D12Resource> table12, visibility12, unshadowed12, bounced12;
    ID3D12Resource* depth12{}; // The depth copy the descriptors point at
    ID3D12Resource* albedo12{}; // The bounce's diffuse colours they point at
    Denoiser shadows, bounce;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    std::array<bool, FramesInFlight> recorded{};
    float milliseconds{};
};

// A mesh's template with its reference tree inverted: a tree is placed by M_tree inverse(M_reference),
// and by the camera's movement since the template was cached.
struct FoliageReference
{
    GeometryCache::Template entry;
    double inverse[3][3], origin[3];

    bool operator==(const FoliageReference&) const = default;
};

// Everything created while a view or feature is on.
struct Gpu
{
    ID3D11DeviceContext* context{};
    ComPtr<ID3D11Device5> device;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11Buffer> streamed;
    ComPtr<ID3D11ShaderResourceView> streamedView;
    ComPtr<ID3D11Texture2D> positions, traced, sunlit, depthCopy;
    ComPtr<ID3D11UnorderedAccessView> positionsAccess, depthCopyAccess;
    ComPtr<ID3D11ShaderResourceView> tracedView, sunlitView, depthView, shadowView, depthCopyView;
    // Soft sun shadows: each pixel's sun ray across the disc, shared with D3D12, and filtered.
    ComPtr<ID3D11Texture2D> sunSoft;
    ComPtr<ID3D11ShaderResourceView> sunSoftView;
    ID3D11ShaderResourceView* sunFiltered{}; // This frame's, when soft
    Denoiser sunDenoiser;
    // The game's G-buffer normal over the view, shared with D3D12 for every feature's rays and filters;
    // 0, unknown, where the game drew sky or in frames without it.
    ComPtr<ID3D11Texture2D> normals;
    ComPtr<ID3D11ShaderResourceView> normalsView;
    ComPtr<ID3D11UnorderedAccessView> normalsAccess;
    bool normalsKnown{}; // This frame's are copied
    ID3D11Resource* depthResource{}; // The game's depth texture behind depthView
    ComPtr<ID3D11Texture2D> shadowCopy; // The game's sun shadow as its resolve passes left it
    UINT tracedWidth{}, tracedHeight{};
    ComPtr<ID3D11ComputeShader> positionShader, depthShader, bakeCopyShader;
    ComPtr<ID3D11VertexShader> fullScreenShader;
    ComPtr<ID3D11PixelShader> compositeShader, injectShader;
    ComPtr<ID3D11BlendState> redOnly;
    ComPtr<ID3D11Buffer> positionConstants, regionConstants, compositeConstants, bakeConstants, bakeLevel;
    // Alpha-tested geometry: streamed vertices, their coordinates for D3D12, and the coverage atlas.
    ComPtr<ID3D11Buffer> maskedStreamed, transformRows, transformDraws, materialDraws;
    ComPtr<ID3D11ShaderResourceView> maskedStreamedView, bakeView, transformRowsView, transformDrawsView, materialDrawsView;
    ComPtr<ID3D11Texture2D> coordinates, coverage, bakeDepth;
    ComPtr<ID3D11UnorderedAccessView> coordinatesAccess, coverageAccess;
    ComPtr<ID3D11DepthStencilView> bakeTarget;
    ComPtr<ID3D11RasterizerState> noCull;
    ComPtr<ID3D11DepthStencilState> lowestLevel;
    ComPtr<ID3D11Resource> sceneDepth; // Last frame's G-buffer depth, where the depth prepass draws
    ComPtr<ID3D11Resource> shadowAtlas; // The sun shadow maps the resolve last read
    UINT atlasWidth{}, atlasHeight{};
    ComPtr<ID3D11Buffer> lightSpace; // This frame's LightRows of the resolve's constants
    ComPtr<ID3D11ShaderResourceView> lightSpaceView;
    ComPtr<ID3D11ShaderResourceView> noCanopy; // Zeros in place of the resolve's canopy map: no soft tree shadow
    // Coverage slots by pixel shader and the view in each texture slot it declares (null in the
    // others), with the frame each was last drawn in; the references keep the key's addresses unique.
    // Slots freed by unused materials are taken first.
    using MaterialKey = std::array<IUnknown*, 1 + 32>;
    struct Material { UINT slot; UINT64 usedIn; std::vector<ComPtr<IUnknown>> references; };
    std::map<MaterialKey, Material> materials;
    std::vector<UINT> freeSlots;
    // Stream output of alpha-tested draws by vertex shader signature and coordinate input, and by the
    // shader pair each draw binds; the references keep the pair's addresses unique.
    std::map<std::string, ComPtr<ID3D11GeometryShader>, std::less<>> coordinateOutput;
    struct ShaderPairOutput { ComPtr<ID3D11VertexShader> vertexShader; ComPtr<ID3D11PixelShader> pixelShader; ComPtr<ID3D11GeometryShader> output; UINT64 usedIn{}; };
    std::map<std::pair<IUnknown*, IUnknown*>, ShaderPairOutput> coordinateOutputByShaders;
    ComPtr<ID3D11Fence> fence;
    UINT64 fenceValue{};
    std::array<ComPtr<ID3D11Buffer>, FramesInFlight> sunStaging;
    std::array<bool, FramesInFlight> sunPending{};
    UINT sunNext{};
    float sun[3]{};
    bool sunFound{};

    ComPtr<IUnknown> wrapper; // ReShade's proxy of the D3D12 device, kept alive beside the native device
    ComPtr<ID3D12Device5> d3d12;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12CommandAllocator>, FramesInFlight> allocators;
    std::array<UINT64, FramesInFlight> submitted{};
    UINT64 frames{};
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> d3d12Fence;
    std::unique_ptr<void, EventCloser> event;
    ComPtr<ID3D12Resource> positions12, traced12, sunlit12, sunSoft12, depthCopy12, normals12, coordinates12, coverage12, vertices, bottom,
        casterBottom, top, scratch, instances;
    ComPtr<ID3D12Resource> cacheTable; // Per frame in flight: the traced cached draws' coordinates and slots
    std::unique_ptr<GeometryCache> cache, templates;
    bool cacheReset{}; // The coverage atlas started again; cached slots are stale
    // Objects drawn with velocity or drawn twice in a frame (drawnBefore), by the frame last drawn in:
    // never cached.
    std::unordered_map<std::uint64_t, UINT64> movers;
    std::unordered_set<std::uint64_t> newMovers;      // Movers since the last trace, which evicts their cached entries
    // Foliage: per transform buffer its transforms, the meshes each instance was last drawn with, and
    // the trees near the camera or seen lately with their sections placed; per mesh the cached template,
    // the instance it shows and the reference the trees were last placed by; the visible lists of every
    // FoliageLatency-th frame, read back one at a time.
    struct TreeMeshes { std::array<std::uint64_t, MaxTreeSections> keys; std::uint32_t count; };
    // A tree's sections placed without the camera's position, and what they were placed from.
    struct LiveTree
    {
        std::uint32_t index;
        TreeMeshes meshes;
        UINT64 loadedFrame;

        std::array<D3D12_RAYTRACING_INSTANCE_DESC, MaxTreeSections> sections;
        std::array<std::array<std::uint32_t, 2>, MaxTreeSections> table;
        std::uint32_t count;
    };
    struct FoliageTrees
    {
        ComPtr<ID3D11Buffer> source, staging;
        std::vector<std::array<float, 12>> transforms;
        std::vector<TreeMeshes> meshes;
        std::vector<UINT64> seenIn; // Per instance: the read-back its meshes come from; 0 while not live
        std::vector<LiveTree> live; // Of many more instances
        bool pending{};
        UINT64 loadedFrame{}, drawnFrame{};
    };
    std::unordered_map<ID3D11Buffer*, FoliageTrees> foliage;
    struct FoliageTemplate { std::uint64_t cacheKey; ID3D11Buffer* transforms; UINT reference; std::optional<FoliageReference> placedBy; };
    std::unordered_map<std::uint64_t, FoliageTemplate> foliageTemplates;
    struct FoliageReadback { ComPtr<ID3D11Buffer> staging; std::vector<Capture::FoliageDraw> draws; bool pending{}; };
    FoliageReadback foliageReadback;
    UINT64 foliageReads{}; // Read-backs taken in so far
    UINT64 foliageSectionsIgnored{}; // Past MaxTreeSections, so far
    // The placed foliage, kept until what it is placed from changes: the instances without the camera's
    // position, nearest first when they may not all fit, their table entries, and the templates they
    // were placed from (cache key and structure).
    bool foliageChanged{}; // What the trees are placed from changed: they are placed again at a frame's first draw
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> foliagePlaced;
    std::vector<std::array<std::uint32_t, 2>> foliageTable;
    std::vector<std::pair<std::uint64_t, D3D12_GPU_VIRTUAL_ADDRESS>> foliageStructures;
    UINT foliageInstances{}; // Traced last frame
    UINT64 bottomSize{}, casterBottomSize{}, scratchSize{};
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    UINT descriptorSize{};
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    double millisecondsPerTick{};
    Timings timings;
    // The add-on's Direct3D 11 passes, and their times in the newest frame the GPU has finished.
    std::unique_ptr<PassTimer> timer;
    PassTimer::Milliseconds passes{};
    TraceConstants lastTrace; // This frame's, which the lights' rays share
    AmbientGpu ambient;
    BounceGpu bounce;
    LightsGpu lights;
};
std::unique_ptr<Gpu> gpu;
std::uint64_t tracedFrames{};

LONGLONG ticksNow()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

float millisecondsSince(LONGLONG ticks)
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<float>(static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency.QuadPart));
}

// The render thread's time in trace(), traceLights() and placing the foliage this frame, and the part
// of it spent waiting for the GPU to finish the frame that last used the same slot.
LONGLONG traceTicks{}, gpuWaitTicks{};
// Parts of traceTicks, as Costs describes them.
LONGLONG foliageTicks{}, instancesTicks{}, flushTicks{}, submitTicks{}, prepareTicks{};
// Adds the time until it goes out of scope to traceTicks, however the scope ends.
struct TraceTime
{
    LONGLONG start = ticksNow();
    ~TraceTime() { traceTicks += ticksNow() - start; }
};
// Splits a stretch of code into parts: each lap adds the time since the last one (or since the start)
// to that part's ticks, and restart() leaves the time since the last lap to no part.
struct Stopwatch
{
    LONGLONG start = ticksNow();

    void lap(LONGLONG& ticks)
    {
        const auto now = ticksNow();
        ticks += now - start;
        start = now;
    }

    void restart() { start = ticksNow(); }
};
LONGLONG lastPresent{};
Costs costs; // Averaged; kept while the GPU resources are released, for the frame time
std::atomic<bool> performanceOverlay{};

// Measuring each feature's real cost (CostMeasurement). Runs on the thread presenting; the settings
// the user had come back at the end.
struct Measurement
{
    struct Setting { const char* removed; Features features; View view; bool paused; };
    std::vector<Setting> settings;
    Features features; // Restored at the end
    View view{};
    bool bypassed{};
    std::size_t step{};
    LONGLONG stepStart{}, timingStart{};
    unsigned timedFrames{};
    std::vector<CostMeasurement::Step> results;
    std::uint64_t finishedAt{};
    bool running{};
} measurement;
// While measuring, the scene is captured and built with every feature off, unless paused.
std::atomic<bool> measurementRequested{}, measuring{};
constexpr float SettleSeconds = 1.5f, TimedSeconds = 2.5f;


void check(HRESULT result, const char* what)
{
    if (FAILED(result))
    {
        char text[32]{};
        std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(what) + " failed (" + text + ").");
    }
}

void fail(std::string_view reason)
{
    failed = true;
    {
        std::lock_guard lock(statusMutex);
        published.failure = reason;
    }
    upscaler::log("Ray tracing stopped: " + std::string(reason));
}

// The features that change the game's image this frame: the enabled ones, unless the A/B key is down.
Features activeFeatures()
{
    Features result;
    if (!bypassed)
    {
        result.sunShadows = sunShadows;
        result.ambientOcclusion = ambientOcclusion;
        result.halfAmbientRays = halfAmbientRays;
        result.replaceCanopyShadows = replaceCanopyShadows;
        result.globalIllumination = globalIllumination;
        result.lightShadows = lightShadows;
        result.softLightShadows = softLightShadows;
        result.lightBounce = lightBounce;
        result.softSunShadows = softSunShadows;
    }
    return result;
}

// The ambient occlusion rays run for the feature and for its views.
bool ambientWanted()
{
    const auto view = selected.load();
    return activeFeatures().ambientOcclusion || view == View::AmbientOcclusion || view == View::AmbientComparison;
}

// The bounced light is traced for the feature and for its view.
bool bounceWanted()
{
    return activeFeatures().globalIllumination || selected == View::GlobalIllumination;
}

// The local lights' soft shadows and bounce are filtered along the game's normals, and the bounce
// takes the game's diffuse colours.
bool lightSurfacesWanted()
{
    const auto features = activeFeatures();
    return features.lightShadows && (features.softLightShadows || features.lightBounce);
}

// Inverts a 4x4 matrix in double precision; false when it is singular.
bool invert(const float (&matrix)[16], float (&inverse)[16])
{
    double rows[4][8]{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            rows[row][column] = matrix[row * 4 + column];
            rows[row][column + 4] = row == column ? 1 : 0;
        }

    for (int column = 0; column < 4; ++column)
    {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) pivot = row;
        if (std::abs(rows[pivot][column]) < 1e-30) return false;

        std::swap(rows[pivot], rows[column]);
        const double scale = 1 / rows[column][column];
        for (auto& value : rows[column]) value *= scale;
        for (int row = 0; row < 4; ++row)
        {
            if (row == column) continue;
            const double factor = rows[row][column];
            for (int k = 0; k < 8; ++k) rows[row][k] -= factor * rows[column][k];
        }
    }

    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            inverse[row * 4 + column] = static_cast<float>(rows[row][column + 4]);
    return true;
}

// The world position from clip x, y, w and 1, which a perspective projection determines.
bool invertXYW(const float (&clipFromWorld)[16], float (&worldFromClipXYW)[16])
{
    float clipXYWFromWorld[16]{};
    for (int row = 0; row < 4; ++row)
    {
        clipXYWFromWorld[row * 4] = clipFromWorld[row * 4];
        clipXYWFromWorld[row * 4 + 1] = clipFromWorld[row * 4 + 1];
        clipXYWFromWorld[row * 4 + 2] = clipFromWorld[row * 4 + 3];
        clipXYWFromWorld[row * 4 + 3] = row == 3 ? 1.0f : 0.0f;
    }
    return invert(clipXYWFromWorld, worldFromClipXYW);
}

// ---- Direct3D 11 ----

ComPtr<ID3DBlob> compile(const char* source, const char* name, const char* profile)
{
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error(std::string("Shader ") + name + " did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string()));
    return code;
}

ComPtr<ID3D11Buffer> constantBuffer(ID3D11Device* device, UINT size)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = size;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&description, nullptr, buffer.GetAddressOf()), "Creating a ray-tracing constant buffer");
    return buffer;
}

// At 4K the video memory manager demoted the capture memory to system memory for minutes at a time,
// making the copy and the builds many times slower; this hint asks it to evict other memory first.
void preferResident(ID3D11Resource* resource)
{
    ComPtr<IDXGIResource> dxgi;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&dxgi)))) dxgi->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_HIGH);
}

// Only textures can be shared from Direct3D 11 to 12, and only when Direct3D 11 creates them.
ComPtr<ID3D11Texture2D> sharedTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, const char* name,
    UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.BindFlags = bindFlags;
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&description, nullptr, texture.GetAddressOf()), name);
    preferResident(texture.Get());
    return texture;
}

template<class Interface> ComPtr<Interface> openShared(ID3D12Device* device, HANDLE handle, const char* name)
{
    ComPtr<Interface> opened;
    const auto result = device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);
    check(result, name);
    return opened;
}

ComPtr<ID3D12Resource> openTexture(ID3D12Device* device, ID3D11Texture2D* texture, const char* name)
{
    ComPtr<IDXGIResource1> resource;
    check(texture->QueryInterface(IID_PPV_ARGS(&resource)), name);
    HANDLE handle{};
    check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle), name);
    return openShared<ID3D12Resource>(device, handle, name);
}

// Runs the add-on's own Direct3D 11 work without disturbing the engine's cached bindings.
template<class Work> void inOwnState(Gpu& g, Work&& work)
{
    ComPtr<ID3DDeviceContextState> gameState;
    g.context4->SwapDeviceContextState(g.state.Get(), gameState.GetAddressOf());
    const auto restore = [&]
    {
        g.context->ClearState();
        g.context4->SwapDeviceContextState(gameState.Get(), nullptr);
    };
    try { work(); }
    catch (...)
    {
        restore();
        throw;
    }
    restore();
}

DXGI_FORMAT depthReadFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The depth the game rendered the G-buffer with.
ID3D11ShaderResourceView* gameDepth(Gpu& g, ID3D11Resource* depth)
{
    if (depth == g.depthResource) return g.depthView.Get();

    g.depthResource = nullptr;
    g.depthView.Reset();
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC description{};
    if (!depth || FAILED(depth->QueryInterface(IID_PPV_ARGS(&texture)))) return nullptr;

    texture->GetDesc(&description);
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = depthReadFormat(description.Format);
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    if (view.Format == DXGI_FORMAT_UNKNOWN || !(description.BindFlags & D3D11_BIND_SHADER_RESOURCE)
        || FAILED(g.device->CreateShaderResourceView(texture.Get(), &view, g.depthView.GetAddressOf())))
        return nullptr;
    g.depthResource = depth;
    return g.depthView.Get();
}

// ---- Direct3D 12 ----

// Video memory is marked high priority for residency because the video memory manager demoted it to
// system memory at 4K (see preferResident); upload and readback heaps live there by design.
ComPtr<ID3D12Resource> buffer(ID3D12Device1* device, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
    D3D12_RESOURCE_FLAGS flags, const char* name)
{
    const D3D12_HEAP_PROPERTIES properties{heap};
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(&resource)), name);

    if (heap == D3D12_HEAP_TYPE_DEFAULT)
    {
        ID3D12Pageable* pageable = resource.Get();
        const auto priority = D3D12_RESIDENCY_PRIORITY_HIGH;
        device->SetResidencyPriority(1, &pageable, &priority);
    }

    return resource;
}

Geometries geometries(const Gpu& g, UINT opaqueFirst, UINT opaque, UINT maskedFirst, UINT masked)
{
    Geometries result;
    const auto add = [&](UINT first, UINT vertices, D3D12_RAYTRACING_GEOMETRY_FLAGS flags)
    {
        if (!vertices) return;
        auto& geometry = result.descs[result.count];
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = flags;
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = vertices;
        geometry.Triangles.VertexBuffer = {g.vertices->GetGPUVirtualAddress() + UINT64{first} * VertexBytes, VertexBytes};
        result.base[result.count++] = first;
    };
    add(opaqueFirst, opaque, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
    // Alpha-tested triangles are non-opaque: the trace accepts them only where their coverage is set.
    add(maskedFirst, masked, D3D12_RAYTRACING_GEOMETRY_FLAG_NONE);
    return result;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomInputs(const Geometries& geometries)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = geometries.count;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = geometries.descs.data();
    return inputs;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topInputs(D3D12_GPU_VIRTUAL_ADDRESS instances, UINT count)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = count;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.InstanceDescs = instances;
    return inputs;
}

// Reads the visible instance lists and transforms the GPU has copied by now; the trees are placed again
// once either lands. A tree seen while it is not live, for the first time or again after it left,
// becomes live.
void readFoliage(Gpu& g)
{
    auto& readback = g.foliageReadback;
    D3D11_MAPPED_SUBRESOURCE lists{};
    if (readback.pending && g.context->Map(readback.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &lists) == S_OK)
    {
        // A tree seen this frame is drawn with this frame's meshes: its level of detail may have changed.
        const auto* indices = static_cast<const std::uint32_t*>(lists.pData);
        const auto read = ++g.foliageReads;
        for (const auto& draw : readback.draws)
        {
            auto trees = g.foliage.find(draw.transforms);
            if (trees == g.foliage.end()) continue;

            for (UINT i = 0; i < draw.instances; ++i)
            {
                const auto index = indices[draw.staged + i] & 0xffffff;
                if (index >= trees->second.meshes.size()) continue;

                auto& meshes = trees->second.meshes[index];
                const auto previous = std::exchange(trees->second.seenIn[index], read);
                if (previous != read)
                {
                    if (!previous) trees->second.live.push_back({index});
                    meshes.count = 0;
                }
                const auto end = meshes.keys.begin() + meshes.count;
                if (std::find(meshes.keys.begin(), end, draw.mesh) != end) continue;

                if (meshes.count == MaxTreeSections) ++g.foliageSectionsIgnored;
                else meshes.keys[meshes.count++] = draw.mesh;
            }
            const auto found = g.foliageTemplates.find(draw.mesh);
            if (draw.templateKey && found != g.foliageTemplates.end() && found->second.cacheKey == draw.templateKey)
                found->second.reference = indices[draw.staged] & 0xffffff;
        }
        g.context->Unmap(readback.staging.Get(), 0);
        readback.pending = false;
        g.foliageChanged = true;
    }
    for (auto& [buffer, trees] : g.foliage)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!trees.pending || g.context->Map(trees.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) continue;

        trees.transforms.resize(trees.meshes.size());
        std::memcpy(trees.transforms.data(), mapped.pData, trees.transforms.size() * FoliageTransformBytes);
        g.context->Unmap(trees.staging.Get(), 0);
        trees.pending = false;
        trees.loadedFrame = g.frames;
        g.foliageChanged = true;
    }
}

std::optional<FoliageReference> foliageReference(const Gpu& g, const Gpu::FoliageTemplate& model, const GeometryCache::Template& entry)
{
    const auto source = g.foliage.find(model.transforms);
    if (source == g.foliage.end() || source->second.transforms.size() <= model.reference) return std::nullopt;

    const auto& reference = source->second.transforms[model.reference];
    const double m[3][3]{{reference[0], reference[1], reference[2]}, {reference[4], reference[5], reference[6]},
        {reference[8], reference[9], reference[10]}};
    const double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::abs(determinant) < 1e-12) return std::nullopt;

    FoliageReference result{entry};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result.inverse[r][c] = (m[(c + 1) % 3][(r + 1) % 3] * m[(c + 2) % 3][(r + 2) % 3] - m[(c + 1) % 3][(r + 2) % 3] * m[(c + 2) % 3][(r + 1) % 3])
                / determinant;
    for (int r = 0; r < 3; ++r) result.origin[r] = reference[r * 4 + 3] + entry.preViewTranslation[r];
    return result;
}

// A section of a tree, without the camera's position: the frame's preViewTranslation is added to the
// translation as the tree is traced.
D3D12_RAYTRACING_INSTANCE_DESC placeSection(const std::array<float, 12>& tree, const FoliageReference& reference)
{
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    for (int r = 0; r < 3; ++r)
    {
        double moved = tree[r * 4 + 3];
        for (int c = 0; c < 3; ++c)
        {
            const double a = tree[r * 4] * reference.inverse[0][c] + tree[r * 4 + 1] * reference.inverse[1][c] + tree[r * 4 + 2] * reference.inverse[2][c];
            instance.Transform[r][c] = static_cast<float>(a);
            moved -= a * reference.origin[c];
        }
        instance.Transform[r][3] = static_cast<float>(moved);
    }
    instance.InstanceMask = FoliageMask;
    instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    instance.AccelerationStructure = reference.entry.structure;
    instance.InstanceID = reference.entry.firstVertex;
    return instance;
}

// Places a live tree's sections, each by the reference its mesh's template was last resolved to.
void placeTree(const Gpu& g, const Gpu::FoliageTrees& trees, Gpu::LiveTree& tree)
{
    const auto& meshes = trees.meshes[tree.index];
    tree.meshes = meshes;
    tree.loadedFrame = trees.loadedFrame;
    tree.count = 0;
    for (UINT s = 0; s < meshes.count; ++s)
    {
        const auto model = g.foliageTemplates.find(meshes.keys[s]);
        if (model == g.foliageTemplates.end() || !model->second.placedBy) continue;

        const auto& reference = *model->second.placedBy;
        tree.sections[tree.count] = placeSection(trees.transforms[tree.index], reference);
        tree.table[tree.count] = {reference.entry.coordinates, reference.entry.slot | TemplateCoordinates};
        ++tree.count;
    }
}

// The trees near the camera or seen lately, each section of each an instance of its mesh's template.
// The view leaves the trees it draws to these copies, at any distance. Bend culls far trees by
// occlusion, so they drop out of its lists now and then: a far tree stays for some seconds after it
// was last seen, or its shadow would come and go with the read-backs' lag. A tree that is neither
// stops being live until it is seen again. Past the capacity, the farthest trees go.
// A tree is placed once, without the camera's position, and again only when its meshes, its
// transforms or one of its meshes' templates change; writeInstances adds the camera's position each
// frame. This runs only when one of those may have changed, every few frames while trees are drawn:
// the camera moves little in between, so the trees are kept and ordered by where it was then.
void placeFoliage(Gpu& g, const Capture& frame)
{
    g.foliagePlaced.clear();
    g.foliageTable.clear();
    g.foliageStructures.clear();

    // Each mesh's template is resolved afresh, noting the meshes whose trees are placed again.
    // Templates the cache has dropped or never added are taken again.
    std::vector<std::uint64_t> changed;
    for (auto model = g.foliageTemplates.begin(); model != g.foliageTemplates.end();)
    {
        auto& [mesh, foliageTemplate] = *model;
        std::optional<FoliageReference> reference;
        if (foliageTemplate.reference != NoInstance)
        {
            const auto entry = g.templates->find(foliageTemplate.cacheKey);
            if (!entry)
            {
                changed.push_back(mesh);
                model = g.foliageTemplates.erase(model);
                continue;
            }
            reference = foliageReference(g, foliageTemplate, *entry);
        }
        if (reference != foliageTemplate.placedBy) changed.push_back(mesh);
        if (reference) g.foliageStructures.push_back({foliageTemplate.cacheKey, reference->entry.structure});
        foliageTemplate.placedBy = reference;
        ++model;
    }
    std::ranges::sort(changed);

    const auto& camera = frame.preViewTranslation;
    struct NearTree { double distance; const Gpu::LiveTree* tree; };
    std::vector<NearTree> nearby;
    std::size_t sections{};
    for (auto& [buffer, trees] : g.foliage)
    {
        std::size_t kept{};
        for (std::size_t t = 0; t < trees.live.size(); ++t)
        {
            const auto i = trees.live[t].index;
            const bool loaded = i < trees.transforms.size();
            double distance{};
            if (loaded)
            {
                const auto& transform = trees.transforms[i];
                const double position[3]{transform[3] + camera[0], transform[7] + camera[1], transform[11] + camera[2]};
                distance = position[0] * position[0] + position[1] * position[1] + position[2] * position[2];
            }
            const bool withinRadius = loaded && distance <= double{FoliageRadius} * FoliageRadius;
            if (!withinRadius && trees.seenIn[i] + FoliageMemory < g.foliageReads)
            {
                trees.seenIn[i] = 0;
                continue;
            }

            if (kept != t) trees.live[kept] = trees.live[t];
            auto& tree = trees.live[kept++];
            if (!loaded) continue;

            // The meshes are compared as a set: the order of the draws they come from may change.
            const auto& meshes = trees.meshes[i];
            const auto end = meshes.keys.begin() + meshes.count;
            const bool meshesChanged = !std::is_permutation(meshes.keys.begin(), end, tree.meshes.keys.begin(),
                tree.meshes.keys.begin() + tree.meshes.count);
            const bool templateChanged = std::any_of(meshes.keys.begin(), end,
                [&](std::uint64_t mesh) { return std::ranges::binary_search(changed, mesh); });
            if (meshesChanged || tree.loadedFrame != trees.loadedFrame || templateChanged) placeTree(g, trees, tree);
            nearby.push_back({distance, &tree});
            sections += tree.count;
        }
        trees.live.resize(kept);
    }
    // Beyond the room every frame has, whatever the cache traces, the nearest come first.
    if (sections > MaxFoliageInstances)
        std::sort(nearby.begin(), nearby.end(), [](const NearTree& a, const NearTree& b) { return a.distance < b.distance; });

    for (const auto& [distance, tree] : nearby)
        for (UINT s = 0; s < tree->count && g.foliagePlaced.size() < MaxInstances - 2; ++s)
        {
            g.foliagePlaced.push_back(tree->sections[s]);
            g.foliageTable.push_back(tree->table[s]);
        }
}

// The game frees the transform buffers of the world tiles it streams out, but the reference held here
// would keep every one ever drawn, with its readback and copies, for the session. A buffer goes once
// none of its trees is live and it has not been drawn for FoliageReloadFrames: a tree that is not live
// is only placed again once seen, which draws its buffer again, and by then its transforms are about
// due to be read again anyway. Without the off-screen geometry nothing is recorded or placed, so every
// buffer goes at once. The templates whose reference instance it holds go too, or a buffer made later
// at its address would place their trees: they are taken again where their meshes are drawn, and the
// trees placed from them are placed again.
void dropUnusedFoliage(Gpu& g)
{
    const auto unused = [&](const auto& entry)
    {
        const auto& trees = entry.second;
        return !offscreenGeometry || (g.frames - trees.drawnFrame > FoliageReloadFrames && trees.live.empty());
    };
    if (!std::erase_if(g.foliage, unused)) return;

    std::vector<std::uint64_t> orphaned;
    for (auto model = g.foliageTemplates.begin(); model != g.foliageTemplates.end();)
    {
        if (g.foliage.contains(model->second.transforms))
        {
            ++model;
            continue;
        }

        orphaned.push_back(model->first);
        model = g.foliageTemplates.erase(model);
    }

    std::ranges::sort(orphaned);
    for (auto& [buffer, trees] : g.foliage)
        for (auto& tree : trees.live)
        {
            const auto end = tree.meshes.keys.begin() + tree.meshes.count;
            if (std::any_of(tree.meshes.keys.begin(), end, [&](std::uint64_t mesh) { return std::ranges::binary_search(orphaned, mesh); }))
                tree = {tree.index};
        }

    g.foliageChanged = true;
}

// Reads the visible lists that have landed and, when what the trees are placed from has changed,
// places them, at the frame's first draw: before anything is captured, while the GPU is still busy
// with the frame before. The trees are placed the frame after a read-back lands, so the two halves of
// the work fall in different frames. Without the camera's position the trace drops the templates; the
// trees wait for them. Unused transform buffers are dropped first, so the trees left to be placed
// again are placed in this frame.
void prepareFoliage(Gpu& g, const Capture& frame)
{
    const TraceTime time;
    Stopwatch stopwatch;
    dropUnusedFoliage(g);
    const bool place = offscreenGeometry && frame.cameraKnown && std::exchange(g.foliageChanged, false);
    readFoliage(g);
    if (place) placeFoliage(g, frame);
    stopwatch.lap(foliageTicks);
}

// The foliage is placed before the trace adds the frame's templates, which may evict some it was
// placed from. Their copies are left out from this frame on; the trees are placed again in the next.
void dropEvictedFoliage(Gpu& g)
{
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> evicted;
    for (const auto& [key, structure] : g.foliageStructures)
    {
        const auto entry = g.templates->find(key);
        if (!entry || entry->structure != structure) evicted.push_back(structure);
    }
    if (evicted.empty()) return;

    std::ranges::sort(evicted);
    std::size_t kept{};
    for (std::size_t i = 0; i < g.foliagePlaced.size(); ++i)
    {
        if (std::ranges::binary_search(evicted, g.foliagePlaced[i].AccelerationStructure)) continue;

        g.foliagePlaced[kept] = g.foliagePlaced[i];
        g.foliageTable[kept++] = g.foliageTable[i];
    }
    g.foliagePlaced.resize(kept);
    g.foliageTable.resize(kept);
    g.foliageChanged = true;
}

// A frame's instances: the view's geometry and the shadow casters, each in its own bottom-level
// structure (possibly empty), then, with the off-screen geometry, the cached draws the view did not
// draw and as much of the placed foliage as there is room for, moved by the frame's camera position.
// Returns their number.
UINT writeInstances(Gpu& g, UINT slot, const float (&preViewTranslation)[3])
{
    D3D12_RAYTRACING_INSTANCE_DESC* instances{};
    std::uint32_t (*table)[2]{};
    check(g.instances->Map(0, nullptr, reinterpret_cast<void**>(&instances)), "Writing the instances");
    check(g.cacheTable->Map(0, nullptr, reinterpret_cast<void**>(&table)), "Writing the cached draws");
    instances += UINT64{slot} * MaxInstances;
    table += UINT64{slot} * (MaxInstances - 2);
    const std::pair<ID3D12Resource*, UINT> structures[]{{g.bottom.Get(), ViewMask}, {g.casterBottom.Get(), SunCasterMask}};
    // The instances' masks and IDs are bit fields: setting one reads the word back, and reading the
    // write-combined upload heap is very slow. Each instance is made aside and copied in whole.
    for (UINT i = 0; i < 2; ++i)
    {
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
        instance.InstanceMask = structures[i].second;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = structures[i].first->GetGPUVirtualAddress();
        instances[i] = instance;
    }
    const auto cached = offscreenGeometry ? g.cache->write(instances + 2, table) : 0;
    g.foliageInstances = offscreenGeometry ? static_cast<UINT>(std::min<std::size_t>(g.foliagePlaced.size(), MaxInstances - 2 - cached)) : 0;
    if (g.foliageInstances)
    {
        for (UINT i = 0; i < g.foliageInstances; ++i)
        {
            auto instance = g.foliagePlaced[i];
            for (int r = 0; r < 3; ++r) instance.Transform[r][3] += preViewTranslation[r];
            instances[2 + cached + i] = instance;
        }
        std::memcpy(table + cached, g.foliageTable.data(), g.foliageInstances * sizeof(*table));
    }
    g.cacheTable->Unmap(0, nullptr);
    g.instances->Unmap(0, nullptr);
    return 2 + cached + g.foliageInstances;
}

void waitFor(Gpu& g, UINT64 value)
{
    if (g.d3d12Fence->GetCompletedValue() >= value) return;

    check(g.d3d12Fence->SetEventOnCompletion(value, g.event.get()), "Waiting for Direct3D 12");
    if (WaitForSingleObject(g.event.get(), 2000) != WAIT_OBJECT_0)
        throw std::runtime_error("Direct3D 12 did not finish tracing within 2 seconds.");
}

void waitIdle(Gpu& g)
{
    waitFor(g, *std::max_element(g.submitted.begin(), g.submitted.end()));
}

// Grows the bottom-level structures as the captures grow. The builds take turns with one scratch
// buffer.
void ensureBuildSpace(Gpu& g, const Geometries& view, const Geometries& casters)
{
    const auto viewInputs = bottomInputs(view), casterInputs = bottomInputs(casters);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO viewNeeded{}, castersNeeded{};
    g.d3d12->GetRaytracingAccelerationStructurePrebuildInfo(&viewInputs, &viewNeeded);
    g.d3d12->GetRaytracingAccelerationStructurePrebuildInfo(&casterInputs, &castersNeeded);
    const auto scratch = std::max(viewNeeded.ScratchDataSizeInBytes, castersNeeded.ScratchDataSizeInBytes);
    if (viewNeeded.ResultDataMaxSizeInBytes <= g.bottomSize && castersNeeded.ResultDataMaxSizeInBytes <= g.casterBottomSize
        && scratch <= g.scratchSize)
        return;

    waitIdle(g);
    const auto grow = [&](ComPtr<ID3D12Resource>& structure, UINT64& size, UINT64 needed, const char* name)
    {
        if (needed <= size) return;

        size = needed;
        structure = buffer(g.d3d12.Get(), size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, name);
    };
    grow(g.bottom, g.bottomSize, viewNeeded.ResultDataMaxSizeInBytes, "Growing the bottom-level structure");
    grow(g.casterBottom, g.casterBottomSize, castersNeeded.ResultDataMaxSizeInBytes, "Growing the shadow casters' structure");
    if (scratch > g.scratchSize)
    {
        g.scratchSize = scratch;
        g.scratch = buffer(g.d3d12.Get(), g.scratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Growing the build scratch buffer");
    }
}

// ReShade wraps Direct3D 12 devices created after it loads, and other add-ons would see this one as
// a game device. The native device keeps the add-on's work out of their events.
ComPtr<ID3D12Device5> createDevice(Gpu& g)
{
    ComPtr<IDXGIDevice> dxgi;
    check(g.device.As(&dxgi), "Finding the DXGI device");
    ComPtr<IDXGIAdapter> adapter;
    check(dxgi->GetAdapter(adapter.GetAddressOf()), "Finding the GPU");
    ComPtr<ID3D12Device> created, native;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created)), "Creating the Direct3D 12 device");
    if (SUCCEEDED(created->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(native.GetAddressOf())))) g.wrapper = created;
    else native = created;

    ComPtr<ID3D12Device5> device;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    if (FAILED(native.As(&device)) || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)))
        || options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
        throw std::runtime_error("This GPU or driver does not support DXR 1.1 ray queries.");
    return device;
}

std::unique_ptr<Gpu> create(ID3D11DeviceContext* context)
{
    auto result = std::make_unique<Gpu>();
    auto& g = *result;
    g.context = context;
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    check(device.As(&g.device), "Finding Direct3D 11.4");
    check(context->QueryInterface(IID_PPV_ARGS(&g.context4)), "Finding the Direct3D 11.4 context");

    D3D11_BUFFER_DESC streamed{};
    streamed.ByteWidth = Capacity * VertexBytes;
    streamed.BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_SHADER_RESOURCE;
    streamed.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    check(g.device->CreateBuffer(&streamed, nullptr, g.streamed.GetAddressOf()), "Creating the stream-output buffer");
    preferResident(g.streamed.Get());
    D3D11_SHADER_RESOURCE_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    raw.BufferEx.NumElements = streamed.ByteWidth / 4;
    raw.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    check(g.device->CreateShaderResourceView(g.streamed.Get(), &raw, g.streamedView.GetAddressOf()), "Creating the stream-output view");
    g.positions = sharedTexture(g.device.Get(), RowLength, Rows, DXGI_FORMAT_R32G32B32A32_FLOAT, "Creating the shared positions texture");
    check(g.device->CreateUnorderedAccessView(g.positions.Get(), nullptr, g.positionsAccess.GetAddressOf()), "Creating the positions view");
    streamed.ByteWidth = MaskedCapacity * MaskedVertexBytes;
    check(g.device->CreateBuffer(&streamed, nullptr, g.maskedStreamed.GetAddressOf()), "Creating the alpha-tested stream-output buffer");
    preferResident(g.maskedStreamed.Get());
    raw.BufferEx.NumElements = streamed.ByteWidth / 4;
    check(g.device->CreateShaderResourceView(g.maskedStreamed.Get(), &raw, g.maskedStreamedView.GetAddressOf()),
        "Creating the alpha-tested stream-output view");
    const D3D11_BUFFER_DESC transformRows{MaxTransformDraws * 4 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
    check(g.device->CreateBuffer(&transformRows, nullptr, g.transformRows.GetAddressOf()), "Creating the coordinate transform rows");
    raw.BufferEx.NumElements = transformRows.ByteWidth / 4;
    check(g.device->CreateShaderResourceView(g.transformRows.Get(), &raw, g.transformRowsView.GetAddressOf()), "Creating the transform rows view");
    constexpr UINT transformDrawBytes = sizeof(TransformDraw);
    const D3D11_BUFFER_DESC transformDraws{MaxTransformDraws * transformDrawBytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, transformDrawBytes};
    check(g.device->CreateBuffer(&transformDraws, nullptr, g.transformDraws.GetAddressOf()), "Creating the coordinate transform draws");
    check(g.device->CreateShaderResourceView(g.transformDraws.Get(), nullptr, g.transformDrawsView.GetAddressOf()),
        "Creating the transform draws view");
    const D3D11_BUFFER_DESC materialDraws{MaxMaterialDraws * 8, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 8};
    check(g.device->CreateBuffer(&materialDraws, nullptr, g.materialDraws.GetAddressOf()), "Creating the material draws");
    check(g.device->CreateShaderResourceView(g.materialDraws.Get(), nullptr, g.materialDrawsView.GetAddressOf()), "Creating the material draws view");
    g.coordinates = sharedTexture(g.device.Get(), RowLength, MaskedCapacity / RowLength, DXGI_FORMAT_R32G32B32A32_FLOAT,
        "Creating the shared texture coordinates");
    check(g.device->CreateUnorderedAccessView(g.coordinates.Get(), nullptr, g.coordinatesAccess.GetAddressOf()), "Creating the coordinates view");
    D3D11_BUFFER_DESC lightSpace{LightRowCount * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
    check(g.device->CreateBuffer(&lightSpace, nullptr, g.lightSpace.GetAddressOf()), "Creating the light space buffer");
    raw.BufferEx.NumElements = LightRowCount * 4;
    check(g.device->CreateShaderResourceView(g.lightSpace.Get(), &raw, g.lightSpaceView.GetAddressOf()), "Creating the light space view");
    const D3D11_TEXTURE2D_DESC canopy{1, 1, 1, 2, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
    const std::uint64_t zeros[2]{};
    const D3D11_SUBRESOURCE_DATA zeroed[2]{{&zeros[0], 8, 8}, {&zeros[1], 8, 8}};
    ComPtr<ID3D11Texture2D> noCanopy;
    check(g.device->CreateTexture2D(&canopy, zeroed, noCanopy.GetAddressOf()), "Creating the empty canopy map");
    check(g.device->CreateShaderResourceView(noCanopy.Get(), nullptr, g.noCanopy.GetAddressOf()), "Creating the empty canopy view");
    g.coverage = sharedTexture(g.device.Get(), SlotSize * AtlasSlots, SlotSize * AtlasSlots, DXGI_FORMAT_R8_UNORM, "Creating the coverage atlas");
    check(g.device->CreateUnorderedAccessView(g.coverage.Get(), nullptr, g.coverageAccess.GetAddressOf()), "Creating the coverage view");
    D3D11_TEXTURE2D_DESC bake{SlotSize * BakeSamples, SlotSize * BakeSamples, 1, 1, DXGI_FORMAT_R32_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE};
    check(g.device->CreateTexture2D(&bake, nullptr, g.bakeDepth.GetAddressOf()), "Creating the coverage bake target");
    const D3D11_DEPTH_STENCIL_VIEW_DESC bakeTarget{DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2D};
    check(g.device->CreateDepthStencilView(g.bakeDepth.Get(), &bakeTarget, g.bakeTarget.GetAddressOf()), "Creating the coverage bake target view");
    D3D11_SHADER_RESOURCE_VIEW_DESC bakeView{};
    bakeView.Format = DXGI_FORMAT_R32_FLOAT;
    bakeView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    bakeView.Texture2D.MipLevels = 1;
    check(g.device->CreateShaderResourceView(g.bakeDepth.Get(), &bakeView, g.bakeView.GetAddressOf()), "Creating the coverage bake view");
    D3D11_RASTERIZER_DESC noCull{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    noCull.DepthClipEnable = TRUE;
    check(g.device->CreateRasterizerState(&noCull, g.noCull.GetAddressOf()), "Creating the coverage bake rasteriser state");
    const D3D11_DEPTH_STENCIL_DESC lowestLevel{TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS};
    check(g.device->CreateDepthStencilState(&lowestLevel, g.lowestLevel.GetAddressOf()), "Creating the coverage bake depth state");

    auto code = compile(shaders::Positions, "ray-tracing-positions", "cs_5_0");
    check(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, g.positionShader.GetAddressOf()),
        "Creating the positions shader");
    code = compile(shaders::Depth, "ray-tracing-depth", "cs_5_0");
    check(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, g.depthShader.GetAddressOf()),
        "Creating the depth copy shader");
    code = compile(shaders::BakeCopy, "ray-tracing-bake-copy", "cs_5_0");
    check(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, g.bakeCopyShader.GetAddressOf()),
        "Creating the coverage copy shader");
    code = compile(shaders::FullScreen, "ray-tracing-full-screen", "vs_5_0");
    check(g.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, g.fullScreenShader.GetAddressOf()),
        "Creating the full-screen shader");
    code = compile(shaders::Inject, "ray-tracing-inject", "ps_5_0");
    check(g.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, g.injectShader.GetAddressOf()),
        "Creating the sun shadow shader");
    code = compile(shaders::Composite, "ray-tracing-composite", "ps_5_0");
    check(g.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, g.compositeShader.GetAddressOf()),
        "Creating the ray-tracing composite shader");
    D3D11_BLEND_DESC redOnly{};
    redOnly.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    check(g.device->CreateBlendState(&redOnly, g.redOnly.GetAddressOf()), "Creating the sun shadow blend state");
    g.positionConstants = constantBuffer(g.device.Get(), sizeof(PositionConstants));
    g.regionConstants = constantBuffer(g.device.Get(), sizeof(RegionConstants));
    g.compositeConstants = constantBuffer(g.device.Get(), sizeof(CompositeConstants));
    g.bakeConstants = constantBuffer(g.device.Get(), sizeof(BakeConstants));
    g.bakeLevel = constantBuffer(g.device.Get(), sizeof(BakeLevel));
    const auto level = g.device->GetFeatureLevel();
    const UINT flags = (g.device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    check(g.device->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, g.state.GetAddressOf()),
        "Creating the ray-tracing device context state");
    check(g.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.fence)), "Creating the shared fence");

    g.d3d12 = createDevice(g);
    D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_DIRECT};
    check(g.d3d12->CreateCommandQueue(&queue, IID_PPV_ARGS(&g.queue)), "Creating the Direct3D 12 queue");
    for (auto& allocator : g.allocators)
        check(g.d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Creating a command allocator");
    check(g.d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocators[0].Get(), nullptr, IID_PPV_ARGS(&g.list)),
        "Creating the command list");
    check(g.list->Close(), "Closing the command list");
    HANDLE fence{};
    check(g.fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence), "Sharing the fence");
    g.d3d12Fence = openShared<ID3D12Fence>(g.d3d12.Get(), fence, "Opening the shared fence");
    g.event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!g.event) throw std::runtime_error("Creating the fence event failed.");
    g.positions12 = openTexture(g.d3d12.Get(), g.positions.Get(), "Opening the shared positions texture");
    g.coordinates12 = openTexture(g.d3d12.Get(), g.coordinates.Get(), "Opening the shared texture coordinates");
    g.coverage12 = openTexture(g.d3d12.Get(), g.coverage.Get(), "Opening the coverage atlas");

    g.vertices = buffer(g.d3d12.Get(), UINT64{Capacity} * VertexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "Creating the vertex buffer");
    g.instances = buffer(g.d3d12.Get(), FramesInFlight * MaxInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "Creating the instance buffer");
    g.cacheTable = buffer(g.d3d12.Get(), FramesInFlight * (MaxInstances - 2) * 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_FLAG_NONE, "Creating the cached draw table");
    const D3D11_BUFFER_DESC foliageIndices{FoliageIndexCapacity * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
    check(g.device->CreateBuffer(&foliageIndices, nullptr, g.foliageReadback.staging.GetAddressOf()), "Creating the foliage readback");
    g.cache = std::make_unique<GeometryCache>(g.d3d12.Get(), CacheVertexCapacity, CacheStructureBytes);
    g.templates = std::make_unique<GeometryCache>(g.d3d12.Get(), TemplateVertexCapacity, TemplateStructureBytes);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom{}, top{};
    // Both bottom-level structures start small and grow with the captures. The inputs point into the
    // geometries, which must outlive them.
    const auto initial = geometries(g, 0, RowLength * 3, 0, 0);
    const auto bottomBuild = bottomInputs(initial), topBuild = topInputs(g.instances->GetGPUVirtualAddress(), MaxInstances);
    g.d3d12->GetRaytracingAccelerationStructurePrebuildInfo(&bottomBuild, &bottom);
    g.d3d12->GetRaytracingAccelerationStructurePrebuildInfo(&topBuild, &top);
    g.bottomSize = g.casterBottomSize = bottom.ResultDataMaxSizeInBytes;
    g.scratchSize = std::max(bottom.ScratchDataSizeInBytes, top.ScratchDataSizeInBytes);
    g.bottom = buffer(g.d3d12.Get(), g.bottomSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the bottom-level structure");
    g.casterBottom = buffer(g.d3d12.Get(), g.casterBottomSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the shadow casters' structure");
    g.top = buffer(g.d3d12.Get(), top.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the top-level structure");
    g.scratch = buffer(g.d3d12.Get(), g.scratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the build scratch buffer");

    check(g.d3d12->CreateRootSignature(0, TraceShader, sizeof(TraceShader), IID_PPV_ARGS(&g.rootSignature)), "Creating the trace root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = g.rootSignature.Get();
    pipeline.CS = {TraceShader, sizeof(TraceShader)};
    check(g.d3d12->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g.pipeline)), "Creating the trace pipeline");
    // The trace's descriptor table: the game's depth, the texture coordinates, the coverage atlas, the
    // traced image, the sun visibility, the cached draws' and templates' vertices, the game's normals
    // and the soft sun rays' visibility. Those sized by the view are written in ensureTraced.
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 9, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(g.d3d12->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&g.descriptors)), "Creating the descriptor heap");
    g.descriptorSize = g.d3d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto handle = g.descriptors->GetCPUDescriptorHandleForHeapStart();
    for (auto* texture : {g.coordinates12.Get(), g.coverage12.Get()})
    {
        handle.ptr += g.descriptorSize;
        g.d3d12->CreateShaderResourceView(texture, nullptr, handle);
    }
    handle.ptr += 3 * g.descriptorSize;
    for (const auto* cache : {g.cache.get(), g.templates.get()})
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC vertices{};
        vertices.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        vertices.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vertices.Buffer.NumElements = cache->capacity();
        vertices.Buffer.StructureByteStride = VertexBytes;
        g.d3d12->CreateShaderResourceView(cache->vertexResource(), &vertices, handle);
        handle.ptr += g.descriptorSize;
    }
    g.timer = std::make_unique<PassTimer>(g.device.Get());
    D3D12_QUERY_HEAP_DESC timestamps{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, FramesInFlight * TimestampsPerFrame};
    check(g.d3d12->CreateQueryHeap(&timestamps, IID_PPV_ARGS(&g.timestamps)), "Creating the timestamp queries");
    g.timestampReadback = buffer(g.d3d12.Get(), FramesInFlight * TimestampsPerFrame * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "Creating the timestamp readback");
    UINT64 frequency{};
    check(g.queue->GetTimestampFrequency(&frequency), "Reading the timestamp frequency");
    g.millisecondsPerTick = 1000.0 / static_cast<double>(frequency);

    upscaler::log("Ray tracing started: room for " + std::to_string(Capacity / 3) + " triangles"
        + (g.wrapper ? "; unwrapped ReShade's Direct3D 12 device." : "."));
    return result;
}

// The traced images, the game's depth and its normals cover the view at render resolution. The depth
// copy and the normals are made again together, so the other features' descriptors point at both
// again when the depth copy changes.
void ensureTraced(Gpu& g, UINT width, UINT height)
{
    if (g.traced && g.tracedWidth == width && g.tracedHeight == height) return;

    waitIdle(g);
    g.traced = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R32G32B32A32_FLOAT, "Creating the traced image");
    g.sunlit = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the sun visibility image");
    g.depthCopy = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, "Creating the shared depth");
    g.normals = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R32_UINT, "Creating the shared normals");
    g.sunSoft = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the soft sun visibility");
    check(g.device->CreateShaderResourceView(g.sunSoft.Get(), nullptr, g.sunSoftView.ReleaseAndGetAddressOf()), "Creating the soft sun visibility view");
    check(g.device->CreateShaderResourceView(g.depthCopy.Get(), nullptr, g.depthCopyView.ReleaseAndGetAddressOf()), "Creating the depth copy view");
    check(g.device->CreateShaderResourceView(g.traced.Get(), nullptr, g.tracedView.ReleaseAndGetAddressOf()), "Creating the traced image view");
    check(g.device->CreateShaderResourceView(g.sunlit.Get(), nullptr, g.sunlitView.ReleaseAndGetAddressOf()), "Creating the sun visibility view");
    check(g.device->CreateUnorderedAccessView(g.depthCopy.Get(), nullptr, g.depthCopyAccess.ReleaseAndGetAddressOf()), "Creating the shared depth view");
    check(g.device->CreateShaderResourceView(g.normals.Get(), nullptr, g.normalsView.ReleaseAndGetAddressOf()), "Creating the shared normals view");
    check(g.device->CreateUnorderedAccessView(g.normals.Get(), nullptr, g.normalsAccess.ReleaseAndGetAddressOf()), "Creating the shared normals view");
    g.traced12 = openTexture(g.d3d12.Get(), g.traced.Get(), "Opening the traced image");
    g.sunlit12 = openTexture(g.d3d12.Get(), g.sunlit.Get(), "Opening the sun visibility image");
    g.depthCopy12 = openTexture(g.d3d12.Get(), g.depthCopy.Get(), "Opening the shared depth");
    g.normals12 = openTexture(g.d3d12.Get(), g.normals.Get(), "Opening the shared normals");
    g.sunSoft12 = openTexture(g.d3d12.Get(), g.sunSoft.Get(), "Opening the soft sun visibility");

    auto handle = g.descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
    depth.Format = DXGI_FORMAT_R32_FLOAT;
    depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth.Texture2D.MipLevels = 1;
    g.d3d12->CreateShaderResourceView(g.depthCopy12.Get(), &depth, handle);
    handle.ptr += 2 * g.descriptorSize;
    for (auto* image : {g.traced12.Get(), g.sunlit12.Get()})
    {
        handle.ptr += g.descriptorSize;
        g.d3d12->CreateUnorderedAccessView(image, nullptr, nullptr, handle);
    }
    handle.ptr += 3 * g.descriptorSize;
    g.d3d12->CreateShaderResourceView(g.normals12.Get(), nullptr, handle);
    handle.ptr += g.descriptorSize;
    g.d3d12->CreateUnorderedAccessView(g.sunSoft12.Get(), nullptr, nullptr, handle);
    g.tracedWidth = width;
    g.tracedHeight = height;
}

// Copies the game's G-buffer normal over the view: the G-buffer's second target, so it does not
// depend on the game's own ambient occlusion, or else the normal that occlusion pass read. Frames
// without either (menus) clear it to 0, unknown.
void prepareNormals(Gpu& g, const Capture& frame)
{
    const auto& source = frame.gbuffer[1] ? frame.gbuffer[1] : frame.normals;
    ComPtr<ID3D11Texture2D> normals;
    D3D11_TEXTURE2D_DESC description{};
    if (source && SUCCEEDED(source.As(&normals))) normals->GetDesc(&description);
    g.normalsKnown = (description.Format == DXGI_FORMAT_R32_UINT || description.Format == DXGI_FORMAT_R32_TYPELESS)
        && description.Width >= static_cast<UINT>(frame.rect[2]) && description.Height >= static_cast<UINT>(frame.rect[3]);
    if (!g.normalsKnown)
    {
        static bool reported{};
        if (normals && !std::exchange(reported, true))
            upscaler::log("The game's G-buffer normal is not an R32_UINT texture covering the view; ambient occlusion, bounced light and "
                "soft light shadows are not traced, and sun rays take the traced normals.");
        constexpr UINT unknown[4]{};
        g.context->ClearUnorderedAccessViewUint(g.normalsAccess.Get(), unknown);
        return;
    }

    const D3D11_BOX view{static_cast<UINT>(frame.rect[0]), static_cast<UINT>(frame.rect[1]), 0, static_cast<UINT>(frame.rect[2]),
        static_cast<UINT>(frame.rect[3]), 1};
    g.context->CopySubresourceRegion(g.normals.Get(), 0, 0, 0, 0, normals.Get(), 0, &view);
}

void readTimings(Gpu& g, UINT slot)
{
    const D3D12_RANGE range{slot * TimestampsPerFrame * sizeof(UINT64), (slot + 1) * TimestampsPerFrame * sizeof(UINT64)};
    void* ticks{};
    check(g.timestampReadback->Map(0, &range, &ticks), "Reading the timestamps");
    const auto* frame = static_cast<const UINT64*>(ticks) + slot * TimestampsPerFrame;
    const auto milliseconds = [&](int from) { return static_cast<float>(static_cast<double>(frame[from + 1] - frame[from]) * g.millisecondsPerTick); };
    g.timings = {milliseconds(0), milliseconds(1), milliseconds(2), milliseconds(3), milliseconds(4), milliseconds(5), milliseconds(6)};
    const D3D12_RANGE nothing{};
    g.timestampReadback->Unmap(0, &nothing);
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &transition);
}

// Copies whole rows of the shared positions texture holding [first, first + count) into the vertex
// buffer, which has the same layout.
void copyRows(Gpu& g, UINT first, UINT count)
{
    if (!count) return;

    const UINT top = first / RowLength, bottom = (first + count + RowLength - 1) / RowLength;
    D3D12_TEXTURE_COPY_LOCATION source{g.positions12.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION destination{g.vertices.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = {UINT64{top} * RowLength * VertexBytes, {DXGI_FORMAT_R32G32B32A32_FLOAT, RowLength, bottom - top, 1, RowLength * VertexBytes}};
    const D3D12_BOX box{0, top, 0, RowLength, bottom, 1};
    g.list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
}

GeometryCache::GeometryCache(ID3D12Device5* device, UINT vertexCapacity, UINT64 structureBytes)
    : device(device), vertexCapacity(vertexCapacity), structureBytes(structureBytes)
{
    vertexBuffer = buffer(device, UINT64{vertexCapacity} * VertexBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE, "Creating the geometry cache");
    coordinateBuffer = buffer(device, UINT64{vertexCapacity} * VertexBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE, "Creating the geometry cache's coordinates");
    structures = buffer(device, structureBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the geometry cache's structures");
    scratch = buffer(device, CacheScratchBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the geometry cache's scratch buffer");
    staging = buffer(device, UINT64{CacheStagingRows} * RowLength * VertexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "Creating the geometry cache's staging buffer");
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS GeometryCache::inputs(const Entry& entry, D3D12_RAYTRACING_GEOMETRY_DESC& geometry) const
{
    geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = entry.masked ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = entry.vertices;
    geometry.Triangles.VertexBuffer = {vertexBuffer->GetGPUVirtualAddress() + entry.vertexOffset * VertexBytes, VertexBytes};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS result{};
    result.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    result.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    result.NumDescs = 1;
    result.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    result.pGeometryDescs = &geometry;
    return result;
}

void GeometryCache::evict(std::list<Entry>::iterator entry)
{
    byKey.erase(entry->key);
    wholeObjects.erase(entry->object);
    entries.erase(entry);
}

void GeometryCache::evictObjects(const std::unordered_set<std::uint64_t>& objects)
{
    if (objects.empty()) return;

    for (auto entry = entries.begin(); entry != entries.end();)
    {
        const auto next = std::next(entry);
        if (objects.contains(entry->object)) evict(entry);
        entry = next;
    }
}

void GeometryCache::markSlots(std::bitset<MaterialSlots>& slots) const
{
    for (const auto& entry : entries)
        if (entry.slot < MaterialSlots) slots.set(entry.slot);
}

GeometryCache::Room GeometryCache::roomOf(const Entry& entry, Ring ring)
{
    return ring == Ring::Vertices ? Room{entry.vertexOffset, entry.vertexOffset + entry.vertices}
        : ring == Ring::Coordinates ? Room{entry.coordinateOffset, entry.coordinateOffset + (entry.masked ? entry.vertices : 0)}
        : Room{entry.structureOffset, entry.structureOffset + entry.structureSize};
}

// The first of `rooms` that overlaps `room`, or null. Rooms apart and sorted by their beginnings are
// sorted by their ends too.
const GeometryCache::Room* GeometryCache::firstOverlap(const std::vector<Room>& rooms, Room room)
{
    const auto found = std::ranges::partition_point(rooms, [&](const Room& other) { return other.end <= room.begin; });
    return found != rooms.end() && found->begin < room.end ? &*found : nullptr;
}

// Per ring, the rooms of the entries standing in for the view or for shadow casters this frame.
GeometryCache::RingRooms GeometryCache::keptRooms(const std::unordered_set<std::uint64_t>& standIns,
    const std::unordered_set<std::uint64_t>& casting) const
{
    RingRooms kept;
    for (const auto& entry : entries)
    {
        if (!standIns.contains(entry.key) && !casting.contains(entry.object)) continue;

        for (const auto ring : {Ring::Vertices, Ring::Coordinates, Ring::Structures})
            if (const auto room = roomOf(entry, ring); room.begin < room.end) kept[static_cast<int>(ring)].push_back(room);
    }
    for (auto& rooms : kept) std::ranges::sort(rooms, {}, &Room::begin);
    return kept;
}

// Takes the next room in a ring. The kept entries' rooms stay: the allocation moves past them. Empty
// when it cannot. The room taken joins the kept ones, so the frame's later additions move past it
// too. The entries in the rooms taken are evicted by plan(), once all are taken.
std::optional<UINT64> GeometryCache::allocate(Ring ring, UINT64 size, RingRooms& kept)
{
    const UINT64 capacity = ring == Ring::Structures ? structureBytes : vertexCapacity;
    auto& head = heads[static_cast<int>(ring)];
    auto& rooms = kept[static_cast<int>(ring)];
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (head + size > capacity) head = 0;
        const auto begin = head;
        if (const auto* standing = firstOverlap(rooms, {begin, begin + size}))
        {
            head = standing->end;
            continue;
        }

        head += size;
        rooms.insert(std::ranges::lower_bound(rooms, begin, {}, &Room::begin), Room{begin, head});
        return begin;
    }
    return std::nullopt;
}

// The entries kept this frame are found once, at the first addition (most frames add nothing), and the
// entries in the rooms taken are evicted once all are taken: each a single walk over the entries. The
// rooms taken join the kept ones as they are taken, so the frame's additions never overwrite each
// other, even when the kept rooms push them around a whole ring.
void GeometryCache::plan(const std::vector<Draw>& draws, UINT maskedStart, bool all, const std::unordered_set<std::uint64_t>& casting,
    const float (&preViewTranslation)[3])
{
    builds.clear();
    tracedEntries.clear();
    std::copy(std::begin(preViewTranslation), std::end(preViewTranslation), current);
    UINT64 addedVertices{}, addedBytes{}, scratchBytes{};
    UINT stagedRows{};
    std::unordered_set<std::uint64_t> standIns;
    for (const auto& draw : draws)
        if (draw.standIn) standIns.insert(draw.key);
    std::optional<RingRooms> kept;
    RingRooms taken;
    for (const auto& draw : draws)
    {
        if (draw.standIn || byKey.contains(draw.key) || builds.size() == MaxNewCachedDraws) continue;

        Entry entry{draw.key, draw.object, draw.vertices, draw.masked ? draw.slot : NoMaterial, draw.masked, draw.isTemplate, 0, 0, 0, 0,
            {preViewTranslation[0], preViewTranslation[1], preViewTranslation[2]}};
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        const auto sizing = inputs(entry, geometry);
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO needed{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&sizing, &needed);
        const auto aligned = [](UINT64 size) { return (size + 255) & ~UINT64{255}; };
        const auto structureSize = aligned(needed.ResultDataMaxSizeInBytes), scratchSize = aligned(needed.ScratchDataSizeInBytes);
        const UINT rows = draw.masked ? (draw.first + draw.vertices - 1) / RowLength - draw.first / RowLength + 1 : 0;
        // The budgets spread new builds over frames. A draw over them on its own, such as a near
        // tree's foliage, is built alone in a frame: it would never be cached otherwise.
        const bool withinBudget = addedVertices + draw.vertices <= vertexCapacity / 8 && addedBytes + structureSize <= structureBytes / 8;
        const bool alone = builds.empty() && draw.vertices <= vertexCapacity / 2 && structureSize <= structureBytes / 2;
        const bool fits = scratchBytes + scratchSize <= CacheScratchBytes && stagedRows + rows <= CacheStagingRows;
        if (!fits || !(withinBudget || alone)) continue;

        // The count still holds the entries the rooms taken so far will evict, so this may evict an
        // entry, or stop, a little early.
        if (entries.size() == MaxCachedDraws)
        {
            if (standIns.contains(entries.front().key) || casting.contains(entries.front().object)) break;

            evict(entries.begin());
        }
        if (!kept) kept = keptRooms(standIns, casting);
        const auto vertexOffset = allocate(Ring::Vertices, draw.vertices, *kept);
        const auto coordinateOffset = draw.masked ? allocate(Ring::Coordinates, draw.vertices, *kept) : std::optional<UINT64>{0};
        const auto structureOffset = allocate(Ring::Structures, structureSize, *kept);
        if (!vertexOffset || !coordinateOffset || !structureOffset) break;

        entry.vertexOffset = *vertexOffset;
        entry.coordinateOffset = *coordinateOffset;
        entry.structureOffset = *structureOffset;
        entry.structureSize = structureSize;
        for (const auto ring : {Ring::Vertices, Ring::Coordinates, Ring::Structures})
            if (const auto room = roomOf(entry, ring); room.begin < room.end) taken[static_cast<int>(ring)].push_back(room);
        entries.push_back(entry);
        byKey[draw.key] = std::prev(entries.end());
        builds.push_back({std::prev(entries.end()), draw.masked ? maskedStart + draw.first : draw.first, draw.first, scratchBytes});
        addedVertices += draw.vertices;
        addedBytes += structureSize;
        scratchBytes += scratchSize;
        stagedRows += rows;
    }
    // The additions are the last entries.
    if (!builds.empty())
    {
        for (auto& rooms : taken) std::ranges::sort(rooms, {}, &Room::begin);
        for (auto entry = entries.begin(); entry != builds.front().entry;)
        {
            const auto next = std::next(entry);
            for (const auto ring : {Ring::Vertices, Ring::Coordinates, Ring::Structures})
            {
                const auto room = roomOf(*entry, ring);
                if (room.begin < room.end && firstOverlap(taken[static_cast<int>(ring)], room))
                {
                    evict(entry);
                    break;
                }
            }
            entry = next;
        }
    }

    // An object is whole once every draw the view drew of it this frame is cached.
    std::unordered_map<std::uint64_t, bool> whole;
    for (const auto& draw : draws)
    {
        const auto [found, added] = whole.try_emplace(draw.object, true);
        found->second = found->second && byKey.contains(draw.key);
    }
    for (const auto& [object, cached] : whole)
    {
        if (!object) continue;

        if (cached) wholeObjects.insert(object);
        else wholeObjects.erase(object);
    }

    std::unordered_set<std::uint64_t> drawn;
    for (const auto& draw : draws) drawn.insert(draw.object);
    for (const auto& entry : entries)
    {
        if (entry.isTemplate) continue;

        if (standIns.contains(entry.key)) tracedEntries.push_back({&entry, ViewMask | CasterMask});
        else if (all || !drawn.contains(entry.object)) tracedEntries.push_back({&entry, CasterMask});
    }
}

void GeometryCache::record(ID3D12GraphicsCommandList4* list, ID3D12Resource* vertices, ID3D12Resource* coordinates)
{
    if (builds.empty()) return;

    // Alpha-tested coordinates are in the shared texture: their whole rows go to the staging buffer.
    std::vector<UINT64> staged(builds.size());
    UINT row{};
    for (std::size_t i = 0; i < builds.size(); ++i)
    {
        const auto& build = builds[i];
        if (!build.entry->masked) continue;

        const UINT top = build.sourceCoordinate / RowLength, bottom = (build.sourceCoordinate + build.entry->vertices - 1) / RowLength + 1;
        D3D12_TEXTURE_COPY_LOCATION source{coordinates, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION destination{staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint = {UINT64{row} * RowLength * VertexBytes, {DXGI_FORMAT_R32G32B32A32_FLOAT, RowLength, bottom - top, 1,
            RowLength * VertexBytes}};
        const D3D12_BOX box{0, top, 0, RowLength, bottom, 1};
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        staged[i] = (UINT64{row} * RowLength + build.sourceCoordinate % RowLength) * VertexBytes;
        row += bottom - top;
    }
    barrier(list, staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(list, vertexBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(list, coordinateBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    for (std::size_t i = 0; i < builds.size(); ++i)
    {
        const auto& entry = *builds[i].entry;
        const auto bytes = UINT64{entry.vertices} * VertexBytes;
        list->CopyBufferRegion(vertexBuffer.Get(), entry.vertexOffset * VertexBytes, vertices, UINT64{builds[i].sourceVertex} * VertexBytes, bytes);
        if (entry.masked) list->CopyBufferRegion(coordinateBuffer.Get(), entry.coordinateOffset * VertexBytes, staging.Get(), staged[i], bytes);
    }
    barrier(list, staging.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(list, vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(list, coordinateBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Each build has its own part of the scratch buffer, so they need no barriers between them.
    for (const auto& build : builds)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC description{};
        description.Inputs = inputs(*build.entry, geometry);
        description.DestAccelerationStructureData = structures->GetGPUVirtualAddress() + build.entry->structureOffset;
        description.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress() + build.scratchOffset;
        list->BuildRaytracingAccelerationStructure(&description, 0, nullptr);
    }
    D3D12_RESOURCE_BARRIER unordered{};
    unordered.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &unordered);
}

UINT GeometryCache::write(D3D12_RAYTRACING_INSTANCE_DESC* instances, std::uint32_t (*table)[2]) const
{
    for (std::size_t i = 0; i < tracedEntries.size(); ++i)
    {
        const auto& [entry, mask] = tracedEntries[i];
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
        for (int axis = 0; axis < 3; ++axis) instance.Transform[axis][3] = current[axis] - entry->preViewTranslation[axis];
        instance.InstanceID = static_cast<UINT>(entry->vertexOffset);
        instance.InstanceMask = mask;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = structures->GetGPUVirtualAddress() + entry->structureOffset;
        instances[i] = instance;
        table[i][0] = static_cast<std::uint32_t>(entry->coordinateOffset);
        // The top bit marks the foliage templates' entries; an opaque draw's slot, NoMaterial, has it set.
        table[i][1] = entry->slot & ~TemplateCoordinates;
    }
    return static_cast<UINT>(tracedEntries.size());
}

std::optional<GeometryCache::Template> GeometryCache::find(std::uint64_t key) const
{
    const auto found = byKey.find(key);
    if (found == byKey.end()) return std::nullopt;

    const auto& entry = *found->second;
    return Template{structures->GetGPUVirtualAddress() + entry.structureOffset, static_cast<std::uint32_t>(entry.vertexOffset),
        static_cast<std::uint32_t>(entry.coordinateOffset), entry.slot,
        {entry.preViewTranslation[0], entry.preViewTranslation[1], entry.preViewTranslation[2]}};
}

void GeometryCache::clear()
{
    entries.clear();
    byKey.clear();
    wholeObjects.clear();
    std::fill(std::begin(heads), std::end(heads), UINT64{});
    builds.clear();
    tracedEntries.clear();
}

// ---- Ambient occlusion ----

void createAmbient(Gpu& g)
{
    auto& a = g.ambient;
    const auto code = compile(shaders::AmbientPack, "ray-tracing-ambient-pack", "cs_5_0");
    check(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, a.packShader.GetAddressOf()),
        "Creating the ambient occlusion packing shader");
    check(g.d3d12->CreateRootSignature(0, AmbientTraceShader, sizeof(AmbientTraceShader), IID_PPV_ARGS(&a.rootSignature)),
        "Creating the ambient occlusion root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = a.rootSignature.Get();
    pipeline.CS = {AmbientTraceShader, sizeof(AmbientTraceShader)};
    check(g.d3d12->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&a.pipeline)), "Creating the ambient occlusion pipeline");
    // The game's depth, the texture coordinates, the coverage atlas, the game's normal and the traced
    // corners, written in ensureAmbient.
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(g.d3d12->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&a.descriptors)), "Creating the ambient occlusion descriptors");
    D3D12_QUERY_HEAP_DESC timestamps{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, FramesInFlight * 2};
    check(g.d3d12->CreateQueryHeap(&timestamps, IID_PPV_ARGS(&a.timestamps)), "Creating the ambient occlusion timestamps");
    a.timestampReadback = buffer(g.d3d12.Get(), FramesInFlight * 2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "Creating the ambient occlusion timestamp readback");
}

// The traced corners cover the view, and the descriptors also point at the depth copy and the normals
// ensureTraced makes; it makes both again together.
void ensureAmbient(Gpu& g, UINT width, UINT height)
{
    auto& a = g.ambient;
    if (a.traced && a.width == width && a.height == height && a.depth12 == g.depthCopy12.Get()) return;

    waitIdle(g);
    if (!a.traced || a.width != width || a.height != height)
    {
        // Direct3D 11 does not share R32G32_UINT; the traced corners use x and y.
        a.traced = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R32G32B32A32_UINT, "Creating the traced ambient occlusion");
        check(g.device->CreateShaderResourceView(a.traced.Get(), nullptr, a.tracedView.ReleaseAndGetAddressOf()),
            "Creating the traced ambient occlusion view");
        a.traced12 = openTexture(g.d3d12.Get(), a.traced.Get(), "Opening the traced ambient occlusion");
        a.width = width;
        a.height = height;
    }

    auto handle = a.descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
    depth.Format = DXGI_FORMAT_R32_FLOAT;
    depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth.Texture2D.MipLevels = 1;
    g.d3d12->CreateShaderResourceView(g.depthCopy12.Get(), &depth, handle);
    for (auto* texture : {g.coordinates12.Get(), g.coverage12.Get(), g.normals12.Get()})
    {
        handle.ptr += g.descriptorSize;
        g.d3d12->CreateShaderResourceView(texture, nullptr, handle);
    }
    handle.ptr += g.descriptorSize;
    g.d3d12->CreateUnorderedAccessView(a.traced12.Get(), nullptr, nullptr, handle);
    a.depth12 = g.depthCopy12.Get();
}

// The ambient occlusion rays run when they are wanted, the game's normals are known this frame
// (prepareNormals) and the game ran its own occlusion pass, which filterAmbient needs to use them.
void prepareAmbient(Gpu& g, const Capture& frame, UINT width, UINT height)
{
    auto& a = g.ambient;
    a.tracing = false;
    if (!ambientWanted() || !g.normalsKnown || !frame.ambient || !frame.normals) return;

    if (!a.pipeline) createAmbient(g);
    ensureAmbient(g, width, height);
    a.tracing = true;
}

// Traces the ambient occlusion once the frame's structures are built, timed on its own.
void recordAmbient(Gpu& g, UINT slot, const TraceConstants& trace)
{
    auto& a = g.ambient;
    a.milliseconds = 0;
    if (a.recorded[slot])
    {
        const D3D12_RANGE range{slot * 2 * sizeof(UINT64), (slot + 1) * 2 * sizeof(UINT64)};
        void* ticks{};
        check(a.timestampReadback->Map(0, &range, &ticks), "Reading the ambient occlusion timestamps");
        const auto* frame = static_cast<const UINT64*>(ticks) + slot * 2;
        a.milliseconds = static_cast<float>(static_cast<double>(frame[1] - frame[0]) * g.millisecondsPerTick);
        const D3D12_RANGE nothing{};
        a.timestampReadback->Unmap(0, &nothing);
    }
    a.recorded[slot] = a.tracing;
    if (!a.tracing) return;

    AmbientConstants constants{};
    std::memcpy(constants.worldFromClip, trace.worldFromClip, sizeof(constants.worldFromClip));
    std::copy(std::begin(trace.size), std::end(trace.size), constants.size);
    std::copy(std::begin(trace.maskedBase), std::end(trace.maskedBase), constants.maskedBase);
    constants.frame = static_cast<std::uint32_t>(g.frames);
    constants.halfRays = activeFeatures().halfAmbientRays;
    auto* list = g.list.Get();
    ID3D12DescriptorHeap* heaps[]{a.descriptors.Get()};
    list->EndQuery(a.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    list->SetComputeRootSignature(a.rootSignature.Get());
    list->SetPipelineState(a.pipeline.Get());
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
    list->SetComputeRootShaderResourceView(1, g.top->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, g.cacheTable->GetGPUVirtualAddress() + UINT64{slot} * (MaxInstances - 2) * 8);
    list->SetComputeRootShaderResourceView(3, g.cache->coordinates());
    list->SetComputeRootShaderResourceView(4, g.templates->coordinates());
    list->SetComputeRootDescriptorTable(5, a.descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((constants.size[0] + 7) / 8, (constants.size[1] + 7) / 8, 1);
    list->EndQuery(a.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    list->ResolveQueryData(a.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, a.timestampReadback.Get(), slot * 2 * sizeof(UINT64));
}

// ---- Global illumination ----

void createBounce(Gpu& g)
{
    auto& b = g.bounce;
    auto code = compile(shaders::BounceSurfaces, "ray-tracing-bounce-surfaces", "cs_5_0");
    check(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, b.surfaceShader.GetAddressOf()),
        "Creating the bounce surfaces shader");
    code = compile(shaders::BounceAccumulate, "ray-tracing-bounce-accumulate", "cs_5_0");
    check(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, b.accumulateShader.GetAddressOf()),
        "Creating the bounce accumulation shader");
    code = compile(shaders::BounceFilter, "ray-tracing-bounce-filter", "cs_5_0");
    check(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, b.filterShader.GetAddressOf()),
        "Creating the bounce filter shader");
    code = compile(shaders::BounceAdd, "ray-tracing-bounce-add", "ps_5_0");
    check(g.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, b.addShader.GetAddressOf()),
        "Creating the bounce shader");
    D3D11_BLEND_DESC additive{};
    additive.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE};
    check(g.device->CreateBlendState(&additive, b.additive.GetAddressOf()), "Creating the bounce blend state");
    b.constants = constantBuffer(g.device.Get(), sizeof(BounceFilterConstants));
    const std::array<float, std::size(LightingRows) * 4> zeros{};
    const D3D11_BUFFER_DESC lighting{static_cast<UINT>(sizeof(zeros)), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
    const D3D11_SUBRESOURCE_DATA zeroed{zeros.data()};
    check(g.device->CreateBuffer(&lighting, &zeroed, b.lighting.GetAddressOf()), "Creating the lighting rows buffer");
    D3D11_SHADER_RESOURCE_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    raw.BufferEx.NumElements = static_cast<UINT>(zeros.size());
    raw.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    check(g.device->CreateShaderResourceView(b.lighting.Get(), &raw, b.lightingView.GetAddressOf()), "Creating the lighting rows view");

    check(g.d3d12->CreateRootSignature(0, BounceTraceShader, sizeof(BounceTraceShader), IID_PPV_ARGS(&b.rootSignature)),
        "Creating the bounce root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = b.rootSignature.Get();
    pipeline.CS = {BounceTraceShader, sizeof(BounceTraceShader)};
    check(g.d3d12->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&b.pipeline)), "Creating the bounce pipeline");
    // The game's depth, the texture coordinates, the coverage atlas, the game's normals and diffuse
    // colours, and the traced bounce. Those sized by the view are written in ensureBounce.
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(g.d3d12->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&b.descriptors)), "Creating the bounce descriptors");
    auto handle = b.descriptors->GetCPUDescriptorHandleForHeapStart();
    for (auto* texture : {g.coordinates12.Get(), g.coverage12.Get()})
    {
        handle.ptr += g.descriptorSize;
        g.d3d12->CreateShaderResourceView(texture, nullptr, handle);
    }
    D3D12_QUERY_HEAP_DESC timestamps{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, FramesInFlight * 2};
    check(g.d3d12->CreateQueryHeap(&timestamps, IID_PPV_ARGS(&b.timestamps)), "Creating the bounce timestamps");
    b.timestampReadback = buffer(g.d3d12.Get(), FramesInFlight * 2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "Creating the bounce timestamp readback");
}

// The textures cover the view, and the views and descriptors also point at the depth copy and the
// normals ensureTraced makes; it makes both again together.
void ensureBounce(Gpu& g, UINT width, UINT height)
{
    auto& b = g.bounce;
    if (b.albedo && b.width == width && b.height == height && b.depth == g.depthCopy.Get()) return;

    waitIdle(g);
    if (!b.albedo || b.width != width || b.height != height)
    {
        b.albedo = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the shared diffuse colours");
        b.traced = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the traced bounce");
        check(g.device->CreateShaderResourceView(b.albedo.Get(), nullptr, b.albedoView.ReleaseAndGetAddressOf()), "Creating the diffuse colour view");
        check(g.device->CreateUnorderedAccessView(b.albedo.Get(), nullptr, b.albedoAccess.ReleaseAndGetAddressOf()), "Creating the diffuse colour view");
        check(g.device->CreateShaderResourceView(b.traced.Get(), nullptr, b.tracedView.ReleaseAndGetAddressOf()), "Creating the traced bounce view");
        b.albedo12 = openTexture(g.d3d12.Get(), b.albedo.Get(), "Opening the shared diffuse colours");
        b.traced12 = openTexture(g.d3d12.Get(), b.traced.Get(), "Opening the traced bounce");
        b.width = width;
        b.height = height;
    }

    check(g.device->CreateShaderResourceView(g.depthCopy.Get(), nullptr, b.depthView.ReleaseAndGetAddressOf()), "Creating the bounce depth view");
    auto handle = b.descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
    depth.Format = DXGI_FORMAT_R32_FLOAT;
    depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth.Texture2D.MipLevels = 1;
    g.d3d12->CreateShaderResourceView(g.depthCopy12.Get(), &depth, handle);
    handle.ptr += 2 * g.descriptorSize;
    for (auto* texture : {g.normals12.Get(), b.albedo12.Get()})
    {
        handle.ptr += g.descriptorSize;
        g.d3d12->CreateShaderResourceView(texture, nullptr, handle);
    }
    handle.ptr += g.descriptorSize;
    g.d3d12->CreateUnorderedAccessView(b.traced12.Get(), nullptr, nullptr, handle);
    b.depth = g.depthCopy.Get();
}

DXGI_FORMAT stencilReadFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// A view of one of the game's textures covering the view, made again when the texture changes;
// DXGI_FORMAT_UNKNOWN views a depth texture's stencil. Null where the texture cannot be read so.
ID3D11ShaderResourceView* gameView(Gpu& g, ComPtr<ID3D11ShaderResourceView>& view, ID3D11Resource* resource, DXGI_FORMAT format,
    const std::int32_t (&rect)[4])
{
    ComPtr<ID3D11Resource> viewed;
    if (view) view->GetResource(viewed.GetAddressOf());
    if (view && viewed.Get() == resource) return view.Get();

    view.Reset();
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC description{};
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return nullptr;

    texture->GetDesc(&description);
    D3D11_SHADER_RESOURCE_VIEW_DESC readable{};
    readable.Format = format == DXGI_FORMAT_UNKNOWN ? stencilReadFormat(description.Format) : format;
    readable.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    readable.Texture2D.MipLevels = 1;
    if (readable.Format == DXGI_FORMAT_UNKNOWN || !(description.BindFlags & D3D11_BIND_SHADER_RESOURCE)
        || description.Width < static_cast<UINT>(rect[2]) || description.Height < static_cast<UINT>(rect[3])
        || FAILED(g.device->CreateShaderResourceView(texture.Get(), &readable, view.GetAddressOf())))
        return nullptr;
    return view.Get();
}

// Copies the game's diffuse colours over the view for the bounce rays and the lights' bounce, when
// they are wanted and the G-buffer has been seen. The rays and the filters also need the game's
// normals (prepareNormals).
void prepareBounce(Gpu& g, const Capture& frame, UINT width, UINT height)
{
    auto& b = g.bounce;
    b.tracing = b.surfaces = false;
    const bool sunBounce = bounceWanted();
    if ((!sunBounce && !lightSurfacesWanted()) || !g.normalsKnown) return;

    if (!b.pipeline) createBounce(g);
    ID3D11ShaderResourceView* inputs[]{gameView(g, b.gbufferView, frame.gbuffer[0].Get(), DXGI_FORMAT_R32G32_UINT, frame.rect),
        gameView(g, b.stencilView, frame.depth.Get(), DXGI_FORMAT_UNKNOWN, frame.rect)};
    if (std::find(std::begin(inputs), std::end(inputs), nullptr) != std::end(inputs))
    {
        static bool reported{};
        if (!std::exchange(reported, true))
            upscaler::log("The game's G-buffer or stencil cannot be read over the view; bounced light and soft light shadows are not traced.");
        return;
    }

    ensureBounce(g, width, height);
    const RegionConstants region{{frame.rect[0], frame.rect[1]}, {width, height}};
    inOwnState(g, [&]
    {
        g.context->UpdateSubresource(g.regionConstants.Get(), 0, nullptr, &region, 0, 0);
        g.context->CSSetShader(b.surfaceShader.Get(), nullptr, 0);
        g.context->CSSetConstantBuffers(0, 1, g.regionConstants.GetAddressOf());
        g.context->CSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
        g.context->CSSetUnorderedAccessViews(0, 1, b.albedoAccess.GetAddressOf(), nullptr);
        g.context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    });
    b.surfaces = true;
    b.tracing = sunBounce;
}

// Traces the bounce once the frame's structures are built, timed on its own.
void recordBounce(Gpu& g, UINT slot, const TraceConstants& trace)
{
    auto& b = g.bounce;
    b.milliseconds = 0;
    if (b.recorded[slot])
    {
        const D3D12_RANGE range{slot * 2 * sizeof(UINT64), (slot + 1) * 2 * sizeof(UINT64)};
        void* ticks{};
        check(b.timestampReadback->Map(0, &range, &ticks), "Reading the bounce timestamps");
        const auto* frame = static_cast<const UINT64*>(ticks) + slot * 2;
        b.milliseconds = static_cast<float>(static_cast<double>(frame[1] - frame[0]) * g.millisecondsPerTick);
        const D3D12_RANGE nothing{};
        b.timestampReadback->Unmap(0, &nothing);
    }
    b.recorded[slot] = b.tracing;
    if (!b.tracing) return;

    BounceConstants constants{};
    std::memcpy(constants.worldFromClip, trace.worldFromClip, sizeof(constants.worldFromClip));
    std::memcpy(constants.clipFromWorld, trace.clipFromWorld, sizeof(constants.clipFromWorld));
    std::copy(std::begin(trace.size), std::end(trace.size), constants.size);
    std::copy(std::begin(trace.maskedBase), std::end(trace.maskedBase), constants.maskedBase);
    std::copy(std::begin(trace.sun), std::end(trace.sun), constants.sun);
    std::memcpy(constants.geometryBase, trace.geometryBase, sizeof(constants.geometryBase));
    constants.frame = static_cast<std::uint32_t>(g.frames);
    auto* list = g.list.Get();
    ID3D12DescriptorHeap* heaps[]{b.descriptors.Get()};
    list->EndQuery(b.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    list->SetComputeRootSignature(b.rootSignature.Get());
    list->SetPipelineState(b.pipeline.Get());
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
    list->SetComputeRootShaderResourceView(1, g.top->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, g.vertices->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, g.cacheTable->GetGPUVirtualAddress() + UINT64{slot} * (MaxInstances - 2) * 8);
    list->SetComputeRootShaderResourceView(4, g.cache->coordinates());
    list->SetComputeRootShaderResourceView(5, g.templates->coordinates());
    list->SetComputeRootShaderResourceView(6, g.cache->vertices());
    list->SetComputeRootShaderResourceView(7, g.templates->vertices());
    list->SetComputeRootDescriptorTable(8, b.descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((constants.size[0] + 7) / 8, (constants.size[1] + 7) / 8, 1);
    list->EndQuery(b.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    list->ResolveQueryData(b.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, b.timestampReadback.Get(), slot * 2 * sizeof(UINT64));
}

// The denoiser's textures cover the view.
void ensureDenoiser(Gpu& g, Denoiser& d, UINT width, UINT height)
{
    if (d.previousDepth && d.width == width && d.height == height) return;

    D3D11_TEXTURE2D_DESC description{width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS};
    for (UINT i = 0; i < 2; ++i)
        for (auto [texture, view, access] : {std::tie(d.history[i], d.historyViews[i], d.historyAccess[i]),
            std::tie(d.filtered[i], d.filteredViews[i], d.filteredAccess[i])})
        {
            check(g.device->CreateTexture2D(&description, nullptr, texture.ReleaseAndGetAddressOf()), "Creating a filter texture");
            check(g.device->CreateShaderResourceView(texture.Get(), nullptr, view.ReleaseAndGetAddressOf()), "Creating a filter view");
            check(g.device->CreateUnorderedAccessView(texture.Get(), nullptr, access.ReleaseAndGetAddressOf()), "Creating a filter view");
        }
    description.Format = DXGI_FORMAT_R32_FLOAT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check(g.device->CreateTexture2D(&description, nullptr, d.previousDepth.ReleaseAndGetAddressOf()), "Creating a filter's previous depth");
    check(g.device->CreateShaderResourceView(d.previousDepth.Get(), nullptr, d.previousDepthView.ReleaseAndGetAddressOf()),
        "Creating a filter's previous depth view");
    d.width = width;
    d.height = height;
    d.historyValid = false;
}

// Accumulates a traced signal over the view into the denoiser's history, up to `maxFrames`, and
// filters it in `passes` passes with taps 1, 2, 4... texels apart, once Direct3D 11 has waited for
// it. A bounce has its lone bright pixels brought down first, and the sun's turns into the scene
// colour's units in its last pass. Filters along the game's normals (prepareNormals) with the bounce's
// shaders. Returns the result.
enum class Signal { SunBounce, LightBounce, Visibility };
ID3D11ShaderResourceView* denoise(Gpu& g, Denoiser& d, const Capture& frame, const float (&worldFromClip)[16], ID3D11ShaderResourceView* traced,
    UINT maxFrames, UINT passes, Signal signal)
{
    auto& b = g.bounce;
    if (!b.pipeline) createBounce(g);
    ensureDenoiser(g, d, g.tracedWidth, g.tracedHeight);
    // History carries over only from the frame before.
    const bool continued = d.historyValid && d.usedIn + 1 == g.frames;
    d.usedIn = g.frames;
    BounceFilterConstants constants{};
    std::memcpy(constants.worldFromClip, worldFromClip, sizeof(constants.worldFromClip));
    // Translated world is world plus the negated camera position, so last frame's is this frame's
    // moved by the camera's movement since.
    std::memcpy(constants.previousClipFromWorld, d.previousClipFromWorld, sizeof(constants.previousClipFromWorld));
    for (int column = 0; column < 4; ++column)
        for (int axis = 0; axis < 3; ++axis)
            constants.previousClipFromWorld[12 + column] += (d.previousTranslation[axis] - frame.preViewTranslation[axis])
                * d.previousClipFromWorld[axis * 4 + column];

    constants.size[0] = d.width;
    constants.size[1] = d.height;
    constants.history = continued && frame.cameraKnown;
    constants.maxFrames = maxFrames;
    constants.clamp = signal != Signal::Visibility;
    const auto previous = d.current ^ 1;
    const UINT groupsX = (d.width + 7) / 8, groupsY = (d.height + 7) / 8;
    inOwnState(g, [&]
    {
        // The output is bound before the inputs: Direct3D 11 does not read what is still bound for writing.
        const auto pass = [&](ID3D11ComputeShader* shader, ID3D11UnorderedAccessView* output, std::initializer_list<ID3D11ShaderResourceView*> inputs)
        {
            g.context->UpdateSubresource(b.constants.Get(), 0, nullptr, &constants, 0, 0);
            g.context->CSSetShader(shader, nullptr, 0);
            g.context->CSSetConstantBuffers(0, 1, b.constants.GetAddressOf());
            g.context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            g.context->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.begin());
            g.context->Dispatch(groupsX, groupsY, 1);
        };
        pass(b.accumulateShader.Get(), d.historyAccess[d.current].Get(), {traced, d.historyViews[previous].Get(), g.depthCopyView.Get(),
            d.previousDepthView.Get()});
        d.output = d.historyViews[d.current].Get();
        for (UINT i = 0; i < passes; ++i)
        {
            constants.step = 1u << i;
            constants.final = signal == Signal::SunBounce && i + 1 == passes;
            pass(b.filterShader.Get(), d.filteredAccess[i % 2].Get(), {d.output, g.depthCopyView.Get(), g.normalsView.Get(), b.lightingView.Get()});
            d.output = d.filteredViews[i % 2].Get();
        }
        g.context->CopyResource(d.previousDepth.Get(), g.depthCopy.Get());
    });
    d.current = previous;
    d.historyValid = frame.cameraKnown;
    std::memcpy(d.previousClipFromWorld, frame.clipFromWorld, sizeof(d.previousClipFromWorld));
    std::memcpy(d.previousTranslation, frame.preViewTranslation, sizeof(d.previousTranslation));
    return d.output;
}

// Accumulates the traced bounce over frames and filters it, once Direct3D 11 has waited for it.
void filterBounce(Gpu& g, const Capture& frame, const float (&worldFromClip)[16])
{
    denoise(g, g.bounce.denoiser, frame, worldFromClip, g.bounce.tracedView.Get(), 24, 3, Signal::SunBounce);
}

// Lighting is about to draw: keeps the rows of its constants the bounced light is scaled by and the
// target it draws the scene colour into.
void readLighting(Gpu& g, Capture& frame)
{
    if (!bounceWanted()) return;

    auto& b = g.bounce;
    if (!b.pipeline) createBounce(g);
    ComPtr<ID3D11Buffer> constants[3]; // By slot, as LightingRows names them
    D3D11_BUFFER_DESC view{}, light{};
    g.context->PSGetConstantBuffers(1, 1, constants[1].GetAddressOf());
    g.context->PSGetConstantBuffers(2, 1, constants[2].GetAddressOf());
    if (constants[1]) constants[1]->GetDesc(&view);
    if (constants[2]) constants[2]->GetDesc(&light);
    if (view.ByteWidth < 54 * 16 || light.ByteWidth < 141 * 16)
    {
        static bool reported{};
        if (!std::exchange(reported, true))
            upscaler::log("The draw that first reads the sun shadow does not have Bend's lighting constants; global illumination is not added.");
        return;
    }

    for (UINT i = 0; i < std::size(LightingRows); ++i)
    {
        const auto [slot, row] = LightingRows[i];
        const D3D11_BOX rows{row * 16, 0, 0, (row + 1) * 16, 1, 1};
        g.context->CopySubresourceRegion(b.lighting.Get(), 0, i * 16, 0, 0, constants[slot].Get(), 0, &rows);
    }
    ComPtr<ID3D11RenderTargetView> target;
    g.context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
    if (!target) return;

    D3D11_RENDER_TARGET_VIEW_DESC description{};
    target->GetDesc(&description);
    frame.sceneColor.Reset();
    target->GetResource(frame.sceneColor.GetAddressOf());
    frame.sceneColorFormat = description.Format;
    frame.bouncePending = activeFeatures().globalIllumination;
}

// Adds the bounced light to the scene colour once every lighting model has drawn.
void addBounce(Gpu& g, Capture& frame)
{
    frame.bouncePending = false;
    auto& b = g.bounce;
    ComPtr<ID3D11Texture2D> target;
    if (!frame.traced || !b.tracing || FAILED(frame.sceneColor.As(&target))) return;

    D3D11_TEXTURE2D_DESC description{};
    target->GetDesc(&description);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = frame.sceneColorFormat;
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> output;
    check(g.device->CreateRenderTargetView(target.Get(), &view, output.GetAddressOf()), "Creating the scene colour view");
    const RegionConstants region{{frame.rect[0], frame.rect[1]}, {b.width, b.height}};
    g.timer->begin(g.context, Pass::BounceAdd);
    inOwnState(g, [&]
    {
        g.context->UpdateSubresource(g.regionConstants.Get(), 0, nullptr, &region, 0, 0);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(description.Width), static_cast<float>(description.Height), 0, 1};
        ID3D11ShaderResourceView* inputs[]{b.denoiser.output, b.albedoView.Get()};
        g.context->RSSetViewports(1, &viewport);
        g.context->OMSetRenderTargets(1, output.GetAddressOf(), nullptr);
        g.context->OMSetBlendState(b.additive.Get(), nullptr, 0xffffffff);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.context->VSSetShader(g.fullScreenShader.Get(), nullptr, 0);
        g.context->PSSetShader(b.addShader.Get(), nullptr, 0);
        g.context->PSSetConstantBuffers(0, 1, g.regionConstants.GetAddressOf());
        g.context->PSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
        g.context->Draw(3, 0);
    });
    g.timer->end(g.context, Pass::BounceAdd);
    frame.bounceAdded = true;
}

// ---- Local lights ----

D3D12_CPU_DESCRIPTOR_HANDLE lightDescriptor(const Gpu& g, UINT index)
{
    auto handle = g.lights.descriptors->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * g.descriptorSize;
    return handle;
}

void createLights(Gpu& g)
{
    auto& l = g.lights;
    auto code = compile(shaders::LightTable, "ray-tracing-light-table", "cs_5_0");
    check(g.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, l.tableShader.GetAddressOf()),
        "Creating the light table shader");
    code = compile(shaders::LightsAdd, "ray-tracing-lights-add", "ps_5_0");
    check(g.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, l.addShader.GetAddressOf()),
        "Creating the light adding shader");
    D3D11_BLEND_DESC additive{};
    additive.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE};
    check(g.device->CreateBlendState(&additive, l.additive.GetAddressOf()), "Creating the light blend state");

    // A light's rows each, then the lighting constants' row 43 (the exposure).
    const D3D11_BUFFER_DESC constants{(MaxLights + 1) * LightConstantRows * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
    check(g.device->CreateBuffer(&constants, nullptr, l.constants.GetAddressOf()), "Creating the light constants");
    D3D11_SHADER_RESOURCE_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    raw.BufferEx.NumElements = constants.ByteWidth / 4;
    raw.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    check(g.device->CreateShaderResourceView(l.constants.Get(), &raw, l.constantsView.GetAddressOf()), "Creating the light constants view");
    l.tableConstants = constantBuffer(g.device.Get(), sizeof(LightTableConstants));
    l.table = sharedTexture(g.device.Get(), LightConstantRows + 1, MaxLights + 1, DXGI_FORMAT_R32G32B32A32_FLOAT, "Creating the light table");
    check(g.device->CreateUnorderedAccessView(l.table.Get(), nullptr, l.tableAccess.GetAddressOf()), "Creating the light table view");
    l.table12 = openTexture(g.d3d12.Get(), l.table.Get(), "Opening the light table");

    // The stand-ins for the game's shadow maps.
    const float farDepth = 1e30f, nearDepth = -1e30f;
    const D3D11_SUBRESOURCE_DATA farFaces[6]{{&farDepth, 4}, {&farDepth, 4}, {&farDepth, 4}, {&farDepth, 4}, {&farDepth, 4}, {&farDepth, 4}};
    const D3D11_SUBRESOURCE_DATA nearFaces[6]{{&nearDepth, 4}, {&nearDepth, 4}, {&nearDepth, 4}, {&nearDepth, 4}, {&nearDepth, 4}, {&nearDepth, 4}};
    D3D11_TEXTURE2D_DESC map{1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> texture;
    check(g.device->CreateTexture2D(&map, farFaces, texture.GetAddressOf()), "Creating the lit shadow map");
    check(g.device->CreateShaderResourceView(texture.Get(), nullptr, l.litMap.GetAddressOf()), "Creating the lit shadow map view");
    map.ArraySize = 6;
    map.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    D3D11_SHADER_RESOURCE_VIEW_DESC cube{};
    cube.Format = DXGI_FORMAT_R32_FLOAT;
    cube.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cube.TextureCube.MipLevels = 1;
    const auto litCube = [&](const D3D11_SUBRESOURCE_DATA* faces)
    {
        ComPtr<ID3D11ShaderResourceView> view;
        check(g.device->CreateTexture2D(&map, faces, texture.ReleaseAndGetAddressOf()), "Creating a lit shadow cube");
        check(g.device->CreateShaderResourceView(texture.Get(), &cube, view.GetAddressOf()), "Creating a lit shadow cube view");
        return view;
    };
    l.litCube = litCube(farFaces);
    l.litCubeNear = litCube(nearFaces);
    const std::uint8_t lit = 255;
    const D3D11_SUBRESOURCE_DATA volumeData{&lit, 1, 1};
    const D3D11_TEXTURE3D_DESC volume{1, 1, 1, 1, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture3D> volumeTexture;
    check(g.device->CreateTexture3D(&volume, &volumeData, volumeTexture.GetAddressOf()), "Creating the lit shadow volume");
    check(g.device->CreateShaderResourceView(volumeTexture.Get(), nullptr, l.litVolume.GetAddressOf()), "Creating the lit shadow volume view");

    check(g.d3d12->CreateRootSignature(0, LightsTraceShader, sizeof(LightsTraceShader), IID_PPV_ARGS(&l.rootSignature)),
        "Creating the light root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = l.rootSignature.Get();
    pipeline.CS = {LightsTraceShader, sizeof(LightsTraceShader)};
    check(g.d3d12->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&l.pipeline)), "Creating the light pipeline");
    // LightDescriptor order. The depth, the outputs and the game's surfaces are written in ensureLights,
    // each slot as it is made.
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, LightDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(g.d3d12->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&l.descriptors)), "Creating the light descriptors");
    const auto at = [&](UINT index) { return lightDescriptor(g, index); };
    g.d3d12->CreateShaderResourceView(g.coordinates12.Get(), nullptr, at(LightDescriptor::Coordinates));
    g.d3d12->CreateShaderResourceView(g.coverage12.Get(), nullptr, at(LightDescriptor::Coverage));
    g.d3d12->CreateShaderResourceView(l.table12.Get(), nullptr, at(LightDescriptor::Table));
    D3D12_SHADER_RESOURCE_VIEW_DESC empty{};
    empty.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    empty.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    empty.Texture2D.MipLevels = 1;
    empty.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    for (UINT slot = 0; slot < MaxLights; ++slot) g.d3d12->CreateShaderResourceView(nullptr, &empty, at(LightDescriptor::Slots + slot));
    empty.Format = DXGI_FORMAT_R32_UINT;
    g.d3d12->CreateShaderResourceView(nullptr, &empty, at(LightDescriptor::Normals));
    empty.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g.d3d12->CreateShaderResourceView(nullptr, &empty, at(LightDescriptor::Albedo));
    D3D12_SHADER_RESOURCE_VIEW_DESC vertices{};
    vertices.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    vertices.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    vertices.Buffer.StructureByteStride = VertexBytes;
    vertices.Buffer.NumElements = Capacity;
    g.d3d12->CreateShaderResourceView(g.vertices.Get(), &vertices, at(LightDescriptor::Vertices));
    vertices.Buffer.NumElements = g.cache->capacity();
    g.d3d12->CreateShaderResourceView(g.cache->vertexResource(), &vertices, at(LightDescriptor::CacheVertices));
    vertices.Buffer.NumElements = g.templates->capacity();
    g.d3d12->CreateShaderResourceView(g.templates->vertexResource(), &vertices, at(LightDescriptor::TemplateVertices));
    D3D12_QUERY_HEAP_DESC timestamps{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, FramesInFlight * 2};
    check(g.d3d12->CreateQueryHeap(&timestamps, IID_PPV_ARGS(&l.timestamps)), "Creating the light timestamps");
    l.timestampReadback = buffer(g.d3d12.Get(), FramesInFlight * 2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "Creating the light timestamp readback");
}

// The traced images cover the view, and the descriptors also point at the depth copy and the normals
// ensureTraced makes (it makes both again together) and at the diffuse colours prepareBounce copies.
void ensureLights(Gpu& g, UINT width, UINT height)
{
    auto& l = g.lights;
    if (l.visibility && l.width == width && l.height == height && l.depth12 == g.depthCopy12.Get() && l.albedo12 == g.bounce.albedo12.Get())
        return;

    waitIdle(g);
    if (!l.visibility || l.width != width || l.height != height)
    {
        const auto make = [&](ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view, ComPtr<ID3D12Resource>& shared)
        {
            texture = sharedTexture(g.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating a traced light image");
            check(g.device->CreateShaderResourceView(texture.Get(), nullptr, view.ReleaseAndGetAddressOf()), "Creating a traced light image view");
            shared = openTexture(g.d3d12.Get(), texture.Get(), "Opening a traced light image");
        };
        make(l.visibility, l.visibilityView, l.visibility12);
        make(l.unshadowed, l.unshadowedView, l.unshadowed12);
        make(l.bounced, l.bouncedView, l.bounced12);
        l.width = width;
        l.height = height;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
    depth.Format = DXGI_FORMAT_R32_FLOAT;
    depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth.Texture2D.MipLevels = 1;
    g.d3d12->CreateShaderResourceView(g.depthCopy12.Get(), &depth, lightDescriptor(g, LightDescriptor::Depth));
    g.d3d12->CreateUnorderedAccessView(l.visibility12.Get(), nullptr, nullptr, lightDescriptor(g, LightDescriptor::Visibility));
    g.d3d12->CreateUnorderedAccessView(l.unshadowed12.Get(), nullptr, nullptr, lightDescriptor(g, LightDescriptor::Unshadowed));
    g.d3d12->CreateUnorderedAccessView(l.bounced12.Get(), nullptr, nullptr, lightDescriptor(g, LightDescriptor::Bounced));
    g.d3d12->CreateShaderResourceView(g.normals12.Get(), nullptr, lightDescriptor(g, LightDescriptor::Normals));
    if (g.bounce.albedo12) g.d3d12->CreateShaderResourceView(g.bounce.albedo12.Get(), nullptr, lightDescriptor(g, LightDescriptor::Albedo));
    l.depth12 = g.depthCopy12.Get();
    l.albedo12 = g.bounce.albedo12.Get();
}

// A slot as large as the game's scene colour, in the format the light draws write it in.
ID3D11RenderTargetView* lightSlot(Gpu& g, int slot, UINT width, UINT height, DXGI_FORMAT format)
{
    auto& l = g.lights;
    D3D11_TEXTURE2D_DESC current{};
    if (l.slots[slot]) l.slots[slot]->GetDesc(&current);
    if (l.slots[slot] && current.Width == width && current.Height == height && current.Format == format) return l.slotTargets[slot].Get();

    waitIdle(g);
    l.slots[slot] = sharedTexture(g.device.Get(), width, height, format, "Creating a light slot", D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    check(g.device->CreateRenderTargetView(l.slots[slot].Get(), nullptr, l.slotTargets[slot].ReleaseAndGetAddressOf()), "Creating a light slot view");
    l.slots12[slot] = openTexture(g.d3d12.Get(), l.slots[slot].Get(), "Opening a light slot");
    g.d3d12->CreateShaderResourceView(l.slots12[slot].Get(), nullptr, lightDescriptor(g, LightDescriptor::Slots + slot));
    return l.slotTargets[slot].Get();
}

// Whether the draw belongs to the game's local lights: a light's volume, or one of its lighting models.
std::optional<LightShader> lightShader(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11VertexShader> shader;
    LightShader kind{};
    UINT size = sizeof(kind);
    context->VSGetShader(shader.GetAddressOf(), nullptr, nullptr);
    if (!shader || FAILED(shader->GetPrivateData(LightShaderTag, &size, &kind))) return std::nullopt;
    return kind;
}

// The stand-in that leaves a texture of the light's draw lit, if it is a shadow map: a depth texture
// other than the scene's, or a baked shadow volume (the only 3D textures lights read). Cube maps are
// read through a comparison sampler, which may pass toward greater depths.
ID3D11ShaderResourceView* litStandIn(const Gpu& g, const Capture& frame, ID3D11ShaderResourceView* view, bool greaterPasses)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    view->GetDesc(&description);
    if (description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D) return g.lights.litVolume.Get();
    if (description.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D && description.ViewDimension != D3D11_SRV_DIMENSION_TEXTURECUBE) return nullptr;

    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    view->GetResource(resource.GetAddressOf());
    D3D11_TEXTURE2D_DESC texel{};
    if (FAILED(resource.As(&texture)) || resource.Get() == frame.depth.Get()) return nullptr;

    texture->GetDesc(&texel);
    if (!(texel.BindFlags & D3D11_BIND_DEPTH_STENCIL)) return nullptr;
    if (description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) return g.lights.litMap.Get();
    return greaterPasses ? g.lights.litCubeNear.Get() : g.lights.litCube.Get();
}

// Draws one lighting model of a game's light into the light's slot with its shadow maps lit, and keeps
// the light's constants the first time. Returns false where the game draws it as it is: no slot left,
// or a target other than the one the lights add to.
bool drawLight(Gpu& g, Capture& frame, ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances, std::uint32_t first,
    std::int32_t baseVertex, std::uint32_t firstInstance, bool indexed)
{
    auto& l = g.lights;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth.GetAddressOf());
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> held; // Releases them
    for (std::size_t i = 0; i < targets.size(); ++i) held[i].Attach(targets[i]);
    ComPtr<ID3D11Resource> target;
    ComPtr<ID3D11Texture2D> texture;
    if (targets[0]) targets[0]->GetResource(target.GetAddressOf());
    D3D11_TEXTURE2D_DESC size{};
    if (FAILED(target.As(&texture))) return false;

    texture->GetDesc(&size);
    D3D11_RECT scissor{0, 0, static_cast<LONG>(size.Width), static_cast<LONG>(size.Height)};
    ComPtr<ID3D11RasterizerState> rasterizer;
    D3D11_RASTERIZER_DESC raster{};
    context->RSGetState(rasterizer.GetAddressOf());
    if (rasterizer) rasterizer->GetDesc(&raster);
    if (raster.ScissorEnable)
    {
        D3D11_RECT rect{};
        UINT rects = 1;
        context->RSGetScissorRects(&rects, &rect);
        scissor = {std::max(rect.left, 0L), std::max(rect.top, 0L), std::min(rect.right, scissor.right), std::min(rect.bottom, scissor.bottom)};
    }
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT reference{};
    context->OMGetDepthStencilState(depthState.GetAddressOf(), &reference);

    // A light draws each lighting model once, picked by the stencil reference, after its volume.
    const auto model = 1u << (reference & 31);
    const bool sameLight = frame.lightOpen && !frame.lightVolumeDrawn && !(frame.lightModels & model) && scissor.left == frame.lightScissor.left
        && scissor.top == frame.lightScissor.top && scissor.right == frame.lightScissor.right && scissor.bottom == frame.lightScissor.bottom;
    D3D11_RENDER_TARGET_VIEW_DESC targetView{};
    targets[0]->GetDesc(&targetView);
    if (!frame.lightTarget)
    {
        frame.lightTarget = target;
        frame.lightTargetFormat = targetView.Format;
    }
    if (!sameLight)
    {
        frame.lightOpen = true;
        frame.lightVolumeDrawn = false;
        frame.lightScissor = scissor;
        frame.lightModels = 0;
        frame.lightSlot = NoLightSlot;
        ComPtr<ID3D11PixelShader> pixelShader;
        UINT declared{}, declaredSize = sizeof(declared);
        context->PSGetShader(pixelShader.GetAddressOf(), nullptr, nullptr);
        const UINT constantSlot = pixelShader && SUCCEEDED(pixelShader->GetPrivateData(ConstantSlots, &declaredSize, &declared)) && (declared & 1u << 5)
            ? 5 : 4;
        ComPtr<ID3D11Buffer> constants;
        D3D11_BUFFER_DESC description{};
        context->PSGetConstantBuffers(constantSlot, 1, constants.GetAddressOf());
        if (constants) constants->GetDesc(&description);
        if (frame.lightRects.size() < MaxLights && target == frame.lightTarget && targetView.Format == frame.lightTargetFormat
            && description.ByteWidth >= LightConstantRows * 16)
        {
            if (!l.pipeline) createLights(g);
            frame.lightSlot = static_cast<int>(frame.lightRects.size());
            frame.lightRects.push_back(scissor);
            auto* slot = lightSlot(g, frame.lightSlot, size.Width, size.Height, targetView.Format);
            constexpr float black[4]{};
            g.context4->ClearView(slot, black, &scissor, 1);
            const D3D11_BOX rows{0, 0, 0, LightConstantRows * 16, 1, 1};
            context->CopySubresourceRegion(l.constants.Get(), 0, frame.lightSlot * LightConstantRows * 16, 0, 0, constants.Get(), 0, &rows);
            // The lights' bounce takes the exposure from the lighting constants all lights share.
            ComPtr<ID3D11Buffer> lighting;
            D3D11_BUFFER_DESC lightingDescription{};
            context->PSGetConstantBuffers(2, 1, lighting.GetAddressOf());
            if (lighting) lighting->GetDesc(&lightingDescription);
            if (!frame.lightSlot && lightingDescription.ByteWidth >= (ExposureRow + 1) * 16)
            {
                const D3D11_BOX exposure{ExposureRow * 16, 0, 0, (ExposureRow + 1) * 16, 1, 1};
                context->CopySubresourceRegion(l.constants.Get(), 0, MaxLights * LightConstantRows * 16, 0, 0, lighting.Get(), 0, &exposure);
            }
        }
        else ++frame.gameLights;
    }
    frame.lightModels |= model;
    if (frame.lightSlot == NoLightSlot) return false;

    std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{};
    context->PSGetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    bool greaterPasses{};
    for (auto* sampler : samplers)
    {
        if (!sampler) continue;

        D3D11_SAMPLER_DESC description{};
        sampler->GetDesc(&description);
        greaterPasses = greaterPasses || (D3D11_DECODE_IS_COMPARISON_FILTER(description.Filter)
            && (description.ComparisonFunc == D3D11_COMPARISON_GREATER || description.ComparisonFunc == D3D11_COMPARISON_GREATER_EQUAL));
        sampler->Release();
    }
    // Lights read their textures from the first few slots.
    std::array<ID3D11ShaderResourceView*, 16> views{};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    auto lit = views;
    bool replaced{};
    for (auto& view : lit)
    {
        auto* standIn = view ? litStandIn(g, frame, view, greaterPasses) : nullptr;
        if (!standIn) continue;

        view = standIn;
        replaced = true;
    }

    auto redirected = targets;
    redirected[0] = l.slotTargets[frame.lightSlot].Get();
    context->OMSetRenderTargets(static_cast<UINT>(redirected.size()), redirected.data(), depth.Get());
    if (replaced) context->PSSetShaderResources(0, static_cast<UINT>(lit.size()), lit.data());
    if (indexed) context->DrawIndexedInstanced(count, instances, first, baseVertex, firstInstance);
    else context->DrawInstanced(count, instances, first, firstInstance);
    // The engine assumes its bindings are still there.
    context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth.Get());
    if (replaced) context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    for (auto* view : views)
        if (view) view->Release();
    frame.lightsPending = true;
    return true;
}

// Traces toward the lights once they have drawn, timed on its own.
void recordLights(Gpu& g, UINT slot, const Capture& frame, std::uint32_t flags)
{
    auto& l = g.lights;
    l.milliseconds = 0;
    if (l.recorded[slot])
    {
        const D3D12_RANGE range{slot * 2 * sizeof(UINT64), (slot + 1) * 2 * sizeof(UINT64)};
        void* ticks{};
        check(l.timestampReadback->Map(0, &range, &ticks), "Reading the light timestamps");
        const auto* times = static_cast<const UINT64*>(ticks) + slot * 2;
        l.milliseconds = static_cast<float>(static_cast<double>(times[1] - times[0]) * g.millisecondsPerTick);
        const D3D12_RANGE nothing{};
        l.timestampReadback->Unmap(0, &nothing);
    }
    l.recorded[slot] = true;

    const auto& trace = g.lastTrace;
    LightConstants constants{};
    std::memcpy(constants.worldFromClip, trace.worldFromClip, sizeof(constants.worldFromClip));
    std::memcpy(constants.clipFromWorld, trace.clipFromWorld, sizeof(constants.clipFromWorld));
    std::copy(std::begin(trace.size), std::end(trace.size), constants.size);
    std::copy(std::begin(trace.maskedBase), std::end(trace.maskedBase), constants.maskedBase);
    constants.origin[0] = frame.rect[0];
    constants.origin[1] = frame.rect[1];
    constants.lightCount = static_cast<std::uint32_t>(frame.lightRects.size());
    std::copy(std::begin(frame.preViewTranslation), std::end(frame.preViewTranslation), constants.translation);
    constants.flags = flags;
    std::memcpy(constants.geometryBase, trace.geometryBase, sizeof(constants.geometryBase));
    constants.frame = static_cast<std::uint32_t>(g.frames);
    auto* list = g.list.Get();
    ID3D12DescriptorHeap* heaps[]{l.descriptors.Get()};
    list->EndQuery(l.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    list->SetComputeRootSignature(l.rootSignature.Get());
    list->SetPipelineState(l.pipeline.Get());
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
    list->SetComputeRootShaderResourceView(1, g.top->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, g.cacheTable->GetGPUVirtualAddress() + UINT64{slot} * (MaxInstances - 2) * 8);
    list->SetComputeRootShaderResourceView(3, g.cache->coordinates());
    list->SetComputeRootShaderResourceView(4, g.templates->coordinates());
    list->SetComputeRootDescriptorTable(5, l.descriptors->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((constants.size[0] + 7) / 8, (constants.size[1] + 7) / 8, 1);
    list->EndQuery(l.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    list->ResolveQueryData(l.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, l.timestampReadback.Get(), slot * 2 * sizeof(UINT64));
}

// Every light has drawn into its slot: Direct3D 12 traces toward them, Direct3D 11 filters the soft
// shadows and the bounce, and the lights that reach each pixel are added to the scene colour.
void traceLights(Gpu& g, Capture& frame)
{
    frame.lightsPending = false;
    frame.lightOpen = false;
    auto& l = g.lights;
    ComPtr<ID3D11Texture2D> target;
    if (frame.lightRects.empty() || FAILED(frame.lightTarget.As(&target))) return;

    const TraceTime time;
    // Both filter along the game's normals, which prepareNormals copied.
    const auto features = activeFeatures();
    const bool surfaces = g.bounce.surfaces && g.bounce.width == g.tracedWidth && g.bounce.height == g.tracedHeight;
    const bool soft = features.softLightShadows && surfaces, bounce = features.lightBounce && surfaces;
    ensureLights(g, g.tracedWidth, g.tracedHeight);
    LightTableConstants table{};
    for (std::size_t i = 0; i < frame.lightRects.size(); ++i)
    {
        const auto& rect = frame.lightRects[i];
        const std::int32_t corners[]{rect.left, rect.top, rect.right, rect.bottom};
        std::copy(std::begin(corners), std::end(corners), table.rects[i]);
    }
    g.timer->begin(g.context, Pass::LightsAdd);
    inOwnState(g, [&]
    {
        g.context->UpdateSubresource(l.tableConstants.Get(), 0, nullptr, &table, 0, 0);
        g.context->CSSetShader(l.tableShader.Get(), nullptr, 0);
        g.context->CSSetConstantBuffers(0, 1, l.tableConstants.GetAddressOf());
        g.context->CSSetShaderResources(0, 1, l.constantsView.GetAddressOf());
        g.context->CSSetUnorderedAccessViews(0, 1, l.tableAccess.GetAddressOf(), nullptr);
        g.context->Dispatch(1, 1, 1);
    });
    g.timer->end(g.context, Pass::LightsAdd);

    // The frame's trace went to this slot's allocator; the lights follow it there.
    g.timer->begin(g.context, Pass::LightsWait);
    const auto drawn = ++g.fenceValue, traced = ++g.fenceValue;
    check(g.context4->Signal(g.fence.Get(), drawn), "Signalling the drawn lights");
    g.context->Flush();
    const auto slot = static_cast<UINT>((g.frames - 1) % FramesInFlight);
    check(g.list->Reset(g.allocators[slot].Get(), nullptr), "Resetting the command list");
    recordLights(g, slot, frame, (soft ? SoftLightShadows : 0) | (bounce ? LightBounce : 0));
    check(g.list->Close(), "Recording the lights");
    check(g.queue->Wait(g.d3d12Fence.Get(), drawn), "Waiting for the drawn lights");
    ID3D12CommandList* lists[]{g.list.Get()};
    g.queue->ExecuteCommandLists(1, lists);
    check(g.queue->Signal(g.d3d12Fence.Get(), traced), "Signalling the traced lights");
    g.submitted[slot] = traced;
    check(g.context4->Wait(g.fence.Get(), traced), "Waiting for the traced lights");
    g.timer->end(g.context, Pass::LightsWait);

    // Soft shadows follow moving lights (the flashlight) within a few frames; the bounce changes slowly.
    auto* visibility = l.visibilityView.Get();
    ID3D11ShaderResourceView* bounced{};
    g.timer->begin(g.context, Pass::LightsFilter);
    if (soft) visibility = denoise(g, l.shadows, frame, g.lastTrace.worldFromClip, visibility, 6, 2, Signal::Visibility);
    if (bounce) bounced = denoise(g, l.bounce, frame, g.lastTrace.worldFromClip, l.bouncedView.Get(), 16, 3, Signal::LightBounce);
    g.timer->end(g.context, Pass::LightsFilter);

    D3D11_TEXTURE2D_DESC description{};
    target->GetDesc(&description);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = frame.lightTargetFormat;
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> output;
    check(g.device->CreateRenderTargetView(target.Get(), &view, output.GetAddressOf()), "Creating the lights' target view");
    const RegionConstants region{{frame.rect[0], frame.rect[1]}, {l.width, l.height}};
    g.timer->begin(g.context, Pass::LightsAdd);
    inOwnState(g, [&]
    {
        g.context->UpdateSubresource(g.regionConstants.Get(), 0, nullptr, &region, 0, 0);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(description.Width), static_cast<float>(description.Height), 0, 1};
        g.context->RSSetViewports(1, &viewport);
        g.context->OMSetRenderTargets(1, output.GetAddressOf(), nullptr);
        g.context->OMSetBlendState(l.additive.Get(), nullptr, 0xffffffff);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.context->VSSetShader(g.fullScreenShader.Get(), nullptr, 0);
        g.context->PSSetShader(l.addShader.Get(), nullptr, 0);
        g.context->PSSetConstantBuffers(0, 1, g.regionConstants.GetAddressOf());
        ID3D11ShaderResourceView* inputs[]{l.unshadowedView.Get(), visibility, bounced, bounced ? g.bounce.albedoView.Get() : nullptr};
        g.context->PSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
        g.context->Draw(3, 0);
    });
    g.timer->end(g.context, Pass::LightsAdd);
    frame.tracedLights = static_cast<unsigned>(frame.lightRects.size());
}

// Only sun shadows and the debug views read the trace shader's sun rays and primary rays; without them
// only the other features' rays run.
void recordTrace(Gpu& g, UINT slot, const Capture& frame, const Geometries& view, const Geometries& casters, const TraceConstants& constants,
    UINT instances, bool sunRays)
{
    auto* list = g.list.Get();
    const auto timestamp = [&](UINT index) { list->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * TimestampsPerFrame + index); };
    // Any UAV barrier also orders the scratch buffer the two builds share.
    D3D12_RESOURCE_BARRIER unordered{};
    unordered.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;

    timestamp(0);
    copyRows(g, 0, frame.opaqueVertices);
    copyRows(g, MaskedEnd - frame.maskedVertices, frame.maskedVertices);
    constexpr auto readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier(list, g.vertices.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readable);
    timestamp(1);

    g.cache->record(list, g.vertices.Get(), g.coordinates12.Get());
    timestamp(2);
    g.templates->record(list, g.vertices.Get(), g.coordinates12.Get());
    timestamp(3);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.DestAccelerationStructureData = g.bottom->GetGPUVirtualAddress();
    build.Inputs = bottomInputs(view);
    build.ScratchAccelerationStructureData = g.scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    list->ResourceBarrier(1, &unordered);
    timestamp(4);
    build.DestAccelerationStructureData = g.casterBottom->GetGPUVirtualAddress();
    build.Inputs = bottomInputs(casters);
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    list->ResourceBarrier(1, &unordered);
    timestamp(5);
    build.DestAccelerationStructureData = g.top->GetGPUVirtualAddress();
    build.Inputs = topInputs(g.instances->GetGPUVirtualAddress() + UINT64{slot} * MaxInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), instances);
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    list->ResourceBarrier(1, &unordered);
    timestamp(6);

    if (sunRays)
    {
        ID3D12DescriptorHeap* heaps[]{g.descriptors.Get()};
        list->SetComputeRootSignature(g.rootSignature.Get());
        list->SetPipelineState(g.pipeline.Get());
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
        list->SetComputeRootShaderResourceView(1, g.top->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, g.vertices->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(3, g.cacheTable->GetGPUVirtualAddress() + UINT64{slot} * (MaxInstances - 2) * 8);
        list->SetComputeRootShaderResourceView(4, g.cache->coordinates());
        list->SetComputeRootShaderResourceView(5, g.templates->coordinates());
        list->SetComputeRootDescriptorTable(6, g.descriptors->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((constants.size[0] + 7) / 8, (constants.size[1] + 7) / 8, 1);
    }
    timestamp(7);
    recordAmbient(g, slot, constants);
    recordBounce(g, slot, constants);
    // The bounce rays read the vertices too.
    barrier(list, g.vertices.Get(), readable, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResolveQueryData(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * TimestampsPerFrame, TimestampsPerFrame,
        g.timestampReadback.Get(), slot * TimestampsPerFrame * sizeof(UINT64));
}

// The caches stand in for the draws they hold, which then never look their material up, and trace
// them out of view, so a material whose slot they still use is kept however long ago it was drawn.
// Earlier frames' traces may still read a freed slot on the GPU, but Direct3D 11 bakes it again only
// after waiting for this frame's.
void dropUnusedMaterials(Gpu& g)
{
    std::bitset<MaterialSlots> cached;
    g.cache->markSlots(cached);
    g.templates->markSlots(cached);
    for (auto material = g.materials.begin(); material != g.materials.end();)
    {
        const auto slot = material->second.slot;
        if (cached[slot] || g.frames - material->second.usedIn <= MaterialMemory)
        {
            ++material;
            continue;
        }

        g.freeSlots.push_back(slot);
        material = g.materials.erase(material);
    }
}

// Direct3D 11 turns the streamed clip positions into world positions and copies the game's depth,
// Direct3D 12 builds and traces, and Direct3D 11 waits on the GPU before it reads the results.
bool trace(Gpu& g, const Capture& frame)
{
    const TraceTime time;
    const auto width = static_cast<UINT>(frame.rect[2] - frame.rect[0]), height = static_cast<UINT>(frame.rect[3] - frame.rect[1]);
    TraceConstants constants{};
    PositionConstants positions{};
    std::memcpy(constants.clipFromWorld, frame.clipFromWorld, sizeof(constants.clipFromWorld));
    // The view's own capture can be empty when the geometry cache stands in for all of it.
    if (!frame.open || !(frame.opaqueVertices + frame.maskedVertices + frame.standInDraws) || !width || !height
        || !invert(frame.clipFromWorld, constants.worldFromClip)
        || !invertXYW(frame.clipFromWorld, positions.worldFromClipXYW))
        return false;

    // Shadow casters follow the view's geometry in both ranges.
    const auto casterOpaqueStart = frame.casterRuns.empty() ? frame.opaqueVertices : frame.casterRuns.front().opaqueStart;
    const auto casterMaskedStart = frame.casterRuns.empty() ? frame.maskedVertices : frame.casterRuns.front().maskedStart;
    const auto maskedStart = MaskedEnd - frame.maskedVertices;
    const auto view = geometries(g, 0, casterOpaqueStart, maskedStart, casterMaskedStart);
    const auto casters = geometries(g, casterOpaqueStart, frame.opaqueVertices - casterOpaqueStart, maskedStart + casterMaskedStart,
        frame.maskedVertices - casterMaskedStart);
    constants.size[0] = width;
    constants.size[1] = height;
    constants.maskedBase[1] = casterMaskedStart;
    std::copy(std::begin(view.base), std::end(view.base), constants.geometryBase[0]);
    std::copy(std::begin(casters.base), std::end(casters.base), constants.geometryBase[1]);
    std::copy(std::begin(g.sun), std::end(g.sun), constants.sun);
    constants.sun[3] = g.sunFound ? 1.0f : 0.0f;
    // The geometry views show the trees' copies too, which stand in for most of the view's foliage.
    constants.primaryMask = selected == View::ShadowCasters ? SunCasterMask | CasterMask | FoliageMask
        : selected == View::Geometry || selected == View::DepthComparison ? ViewMask | FoliageMask : ViewMask;
    constants.offscreenCheck = selected == View::OffscreenShadows;
    Stopwatch stopwatch;
    ensureTraced(g, width, height);
    ensureBuildSpace(g, view, casters);
    g.timer->begin(g.context, Pass::Prepare);
    prepareNormals(g, frame);
    prepareAmbient(g, frame, width, height);
    prepareBounce(g, frame, width, height);
    auto* depth = gameDepth(g, frame.depth.Get());
    // Sun rays start along the game's normal. Where the game's depth or normal could not be read, they
    // start from the primary rays' hits and normals, which the debug views show too.
    constants.primaryRays = selected != View::Off || !depth || !g.normalsKnown;
    // Soft shadows are filtered along the game's normals.
    const auto features = activeFeatures();
    constants.softShadows = features.sunShadows && features.softSunShadows && g.normalsKnown;
    constants.frame = static_cast<std::uint32_t>(g.frames);
    inOwnState(g, [&]
    {
        std::memcpy(positions.worldFromClip, constants.worldFromClip, sizeof(positions.worldFromClip));
        positions.opaqueCount = frame.opaqueVertices;
        positions.maskedCount = frame.maskedVertices;
        positions.maskedStart = maskedStart;
        positions.casterRunCount = static_cast<std::uint32_t>(frame.casterRuns.size());
        for (std::size_t i = 0; i < frame.casterRuns.size(); ++i)
        {
            const auto& run = frame.casterRuns[i];
            const std::uint32_t values[]{run.opaqueStart, run.maskedStart, run.map, 0};
            std::copy(std::begin(values), std::end(values), positions.casterRuns[i]);
            positions.casterDepths[i][0] = run.minDepth;
            positions.casterDepths[i][1] = run.maxDepth;
        }
        positions.atlasSize[0] = static_cast<float>(g.atlasWidth);
        positions.atlasSize[1] = static_cast<float>(g.atlasHeight);
        positions.transformCount = static_cast<std::uint32_t>(frame.transformDraws.size());
        if (positions.transformCount)
        {
            const D3D11_BOX used{0, 0, 0, static_cast<UINT>(frame.transformDraws.size() * sizeof(TransformDraw)), 1, 1};
            g.context->UpdateSubresource(g.transformDraws.Get(), 0, &used, frame.transformDraws.data(), 0, 0);
        }
        // The capture stops at MaxMaterialDraws.
        positions.materialDrawCount = static_cast<std::uint32_t>(frame.materialDraws.size());
        if (positions.materialDrawCount)
        {
            const D3D11_BOX used{0, 0, 0, positions.materialDrawCount * 8, 1, 1};
            g.context->UpdateSubresource(g.materialDraws.Get(), 0, &used, frame.materialDraws.data(), 0, 0);
        }
        g.context->UpdateSubresource(g.positionConstants.Get(), 0, nullptr, &positions, 0, 0);
        g.context->CSSetShader(g.positionShader.Get(), nullptr, 0);
        g.context->CSSetConstantBuffers(0, 1, g.positionConstants.GetAddressOf());
        ID3D11ShaderResourceView* streams[]{g.streamedView.Get(), g.maskedStreamedView.Get(), g.lightSpaceView.Get(), g.transformDrawsView.Get(),
            g.transformRowsView.Get(), g.materialDrawsView.Get()};
        ID3D11UnorderedAccessView* outputs[]{g.positionsAccess.Get(), g.coordinatesAccess.Get()};
        g.context->CSSetShaderResources(0, static_cast<UINT>(std::size(streams)), streams);
        g.context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
        g.context->Dispatch((frame.opaqueVertices + frame.maskedVertices + 1023) / 1024, 1, 1);
        g.context->CSSetUnorderedAccessViews(0, 2, std::array<ID3D11UnorderedAccessView*, 2>{}.data(), nullptr);

        // Without the game's depth, sun rays start from the traced hits.
        constexpr float none[4]{};
        if (!depth)
        {
            g.context->ClearUnorderedAccessViewFloat(g.depthCopyAccess.Get(), none);
            return;
        }
        const RegionConstants region{{frame.rect[0], frame.rect[1]}, {width, height}};
        g.context->UpdateSubresource(g.regionConstants.Get(), 0, nullptr, &region, 0, 0);
        g.context->CSSetShader(g.depthShader.Get(), nullptr, 0);
        g.context->CSSetConstantBuffers(0, 1, g.regionConstants.GetAddressOf());
        g.context->CSSetShaderResources(0, 1, &depth);
        g.context->CSSetUnorderedAccessViews(0, 1, g.depthCopyAccess.GetAddressOf(), nullptr);
        g.context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    });
    g.timer->end(g.context, Pass::Prepare);
    stopwatch.lap(prepareTicks);
    // From here Direct3D 11 waits for Direct3D 12.
    g.timer->begin(g.context, Pass::Wait);
    const auto geometryReady = ++g.fenceValue, traced = ++g.fenceValue;
    check(g.context4->Signal(g.fence.Get(), geometryReady), "Signalling the captured geometry");
    g.context->Flush();
    stopwatch.lap(flushTicks);

    const auto slot = static_cast<UINT>(g.frames % FramesInFlight);
    const auto waitStart = ticksNow();
    waitFor(g, g.submitted[slot]);
    gpuWaitTicks += ticksNow() - waitStart;
    if (g.submitted[slot]) readTimings(g, slot);
    // Without the camera's position cached geometry cannot be placed, and without the off-screen
    // geometry it is not traced: the caches are emptied and plan nothing.
    const bool caching = offscreenGeometry && frame.cameraKnown;
    if (std::exchange(g.cacheReset, false) || !caching)
    {
        g.cache->clear();
        g.templates->clear();
        g.foliageTemplates.clear();
        // The live trees' sections point at the dropped templates: placed from nothing, they are placed again.
        for (auto& [buffer, trees] : g.foliage)
            for (auto& tree : trees.live) tree = {tree.index};
        g.foliagePlaced.clear();
        g.foliageTable.clear();
        g.foliageStructures.clear();
        g.foliageChanged = true;
    }

    // Objects that started moving leave the cache: their copies would stay where they were seen. So do
    // objects drawn twice in a frame, whose one copy would stand in for both. Once forgotten, an object
    // is cached when drawn still and once, and leaves again when it moves or repeats.
    g.cache->evictObjects(g.newMovers);
    g.newMovers.clear();
    if (g.frames % MoverMemory == 0) std::erase_if(g.movers, [&](const auto& mover) { return g.frames - mover.second > MoverMemory; });

    // The frame's alpha-tested draws looked their materials up in this frame (g.frames moves on only
    // after the trace), so the slots they carry are not freed.
    if (g.frames % MaterialMemory == 0)
    {
        dropUnusedMaterials(g);
        std::erase_if(g.coordinateOutputByShaders, [&](const auto& pair) { return g.frames - pair.second.usedIn > MaterialMemory; });
    }

    // The shadow caster and off-screen shadow views show everything sun rays can see beyond the view,
    // on screen too.
    const std::vector<GeometryCache::Draw> none;
    const std::unordered_set<std::uint64_t> noObjects;
    const bool everything = selected == View::ShadowCasters || selected == View::OffscreenShadows;
    stopwatch.restart();
    g.cache->plan(caching ? frame.cacheDraws : none, maskedStart, everything, frame.casting, frame.preViewTranslation);
    const auto templatesHeld = g.templates->size();
    g.templates->plan(caching ? frame.templateDraws : none, maskedStart, false, noObjects, frame.preViewTranslation);
    // Templates are only evicted to make room for new ones.
    if (g.templates->size() < templatesHeld + g.templates->building().first) dropEvictedFoliage(g);
    const auto instances = writeInstances(g, slot, frame.preViewTranslation);
    // The visible foliage lists of every FoliageLatency-th frame are read back once the GPU has copied them.
    if (!frame.foliageDraws.empty())
    {
        g.foliageReadback.draws = frame.foliageDraws;
        g.foliageReadback.pending = true;
    }
    stopwatch.lap(instancesTicks);
    check(g.allocators[slot]->Reset(), "Resetting a command allocator");
    check(g.list->Reset(g.allocators[slot].Get(), nullptr), "Resetting the command list");
    g.lastTrace = constants;
    recordTrace(g, slot, frame, view, casters, constants, instances, sunShadows || selected != View::Off);
    check(g.list->Close(), "Recording the trace");
    check(g.queue->Wait(g.d3d12Fence.Get(), geometryReady), "Waiting for the captured geometry");
    ID3D12CommandList* lists[]{g.list.Get()};
    g.queue->ExecuteCommandLists(1, lists);
    check(g.queue->Signal(g.d3d12Fence.Get(), traced), "Signalling the traced image");
    stopwatch.lap(submitTicks);
    g.submitted[slot] = traced;
    ++g.frames;
    check(g.context4->Wait(g.fence.Get(), traced), "Waiting for the traced image");
    g.timer->end(g.context, Pass::Wait);
    if (g.bounce.tracing)
    {
        g.timer->begin(g.context, Pass::BounceFilter);
        filterBounce(g, frame, constants.worldFromClip);
        g.timer->end(g.context, Pass::BounceFilter);
    }
    // The sun moves slowly, so soft shadows keep more frames than the lights'.
    g.sunFiltered = nullptr;
    if (constants.softShadows)
    {
        g.timer->begin(g.context, Pass::SunFilter);
        g.sunFiltered = denoise(g, g.sunDenoiser, frame, constants.worldFromClip, g.sunSoftView.Get(), 12, 2, Signal::Visibility);
        g.timer->end(g.context, Pass::SunFilter);
    }
    return true;
}

// Adds the traced sun shadows to the game's shadow target before lighting reads it.
void inject(Gpu& g, Capture& frame)
{
    D3D11_TEXTURE2D_DESC description{};
    frame.shadow->GetDesc(&description);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = frame.shadowFormat;
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> target;
    check(g.device->CreateRenderTargetView(frame.shadow.Get(), &view, target.GetAddressOf()), "Creating the sun shadow target view");
    const RegionConstants region{{frame.rect[0], frame.rect[1]}, {g.tracedWidth, g.tracedHeight}};
    g.timer->begin(g.context, Pass::SunInject);
    inOwnState(g, [&]
    {
        g.context->UpdateSubresource(g.regionConstants.Get(), 0, nullptr, &region, 0, 0);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(description.Width), static_cast<float>(description.Height), 0, 1};
        ID3D11ShaderResourceView* inputs[]{g.sunlitView.Get(), g.shadowView.Get(), g.sunFiltered ? g.sunFiltered : g.sunlitView.Get()};
        g.context->RSSetViewports(1, &viewport);
        g.context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        g.context->OMSetBlendState(g.redOnly.Get(), nullptr, 0xffffffff);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.context->VSSetShader(g.fullScreenShader.Get(), nullptr, 0);
        g.context->PSSetShader(g.injectShader.Get(), nullptr, 0);
        g.context->PSSetConstantBuffers(0, 1, g.regionConstants.GetAddressOf());
        g.context->PSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
        g.context->Draw(3, 0);
    });
    g.timer->end(g.context, Pass::SunInject);
    frame.shadowsInjected = true;
}

void composite(Gpu& g, const Capture& frame, ID3D11Texture2D* backBuffer, View view)
{
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    ComPtr<ID3D11RenderTargetView> target;
    check(g.device->CreateRenderTargetView(backBuffer, nullptr, target.GetAddressOf()), "Creating the back buffer view");
    const bool comparesDepth = view == View::DepthComparison || view == View::ShadowCasters;
    auto* depth = comparesDepth ? gameDepth(g, frame.depth.Get()) : nullptr;
    auto* shadow = view == View::ShadowComparison && frame.shadowCopied ? g.shadowView.Get() : nullptr;
    const bool showsAmbient = view == View::AmbientOcclusion || view == View::AmbientComparison;
    auto* ambient = showsAmbient && frame.ambientFiltered ? g.ambient.filteredView.Get() : nullptr;
    const bool showsBounce = view == View::GlobalIllumination;
    auto* bounce = showsBounce && g.bounce.tracing ? g.bounce.denoiser.output : nullptr;
    static bool reported{}, ambientReported{}, bounceReported{};
    if (((comparesDepth && !depth) || (view == View::ShadowComparison && !shadow)) && !std::exchange(reported, true))
        upscaler::log("Ray tracing cannot read the game's depth or sun shadow; the comparison shows the traced geometry instead.");
    if (showsAmbient && !ambient && !std::exchange(ambientReported, true))
        upscaler::log("The frame has no traced ambient occlusion in the game's packing; the view shows the traced geometry instead.");
    if (showsBounce && !bounce && !std::exchange(bounceReported, true))
        upscaler::log("The frame has no traced global illumination; the view shows the traced geometry instead.");
    auto mode = CompositeMode::Geometry;
    if (depth) mode = CompositeMode::DepthComparison;
    else if (shadow) mode = CompositeMode::ShadowComparison;
    else if (view == View::SunShadows) mode = CompositeMode::SunLight;
    else if (view == View::OffscreenShadows) mode = CompositeMode::OffscreenShadows;
    else if (ambient) mode = view == View::AmbientOcclusion ? CompositeMode::AmbientOcclusion : CompositeMode::AmbientComparison;
    else if (bounce) mode = CompositeMode::GlobalIllumination;
    const auto outputWidth = std::max(1, frame.unscaled[2] - frame.unscaled[0]), outputHeight = std::max(1, frame.unscaled[3] - frame.unscaled[1]);
    const CompositeConstants constants{{frame.unscaled[0], frame.unscaled[1]}, {frame.rect[0], frame.rect[1]},
        {static_cast<float>(g.tracedWidth) / static_cast<float>(outputWidth), static_cast<float>(g.tracedHeight) / static_cast<float>(outputHeight)},
        mode, 0};
    inOwnState(g, [&]
    {
        g.context->UpdateSubresource(g.compositeConstants.Get(), 0, nullptr, &constants, 0, 0);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(description.Width), static_cast<float>(description.Height), 0, 1};
        ID3D11ShaderResourceView* inputs[]{g.tracedView.Get(), depth, shadow, g.sunlitView.Get(), ambient, ambient ? g.ambient.gameView.Get() : nullptr,
            ambient ? g.normalsView.Get() : nullptr, bounce, bounce ? g.bounce.albedoView.Get() : nullptr};
        g.context->RSSetViewports(1, &viewport);
        g.context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.context->VSSetShader(g.fullScreenShader.Get(), nullptr, 0);
        g.context->PSSetShader(g.compositeShader.Get(), nullptr, 0);
        g.context->PSSetConstantBuffers(0, 1, g.compositeConstants.GetAddressOf());
        g.context->PSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
        g.context->Draw(3, 0);
    });
}

// Stream output writes three vertices per triangle, instance after instance. Strips with cut
// indices would write fewer, leaving stale vertices at the end of their range.
std::uint64_t streamedVertices(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances)
{
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    std::uint64_t triangles{};
    if (topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) triangles = count / 3;
    else if (topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && count >= 3) triangles = count - 2;
    return triangles * 3 * instances;
}

// Decals blend into the G-buffer. Opaque geometry writes depth or matches the depth prepass exactly;
// the sky dome is tested against the far plane without writing depth, and full-screen passes into
// the G-buffer do not test depth at all.
bool opaqueGeometry(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    context->OMGetBlendState(blend.GetAddressOf(), nullptr, nullptr);
    context->OMGetDepthStencilState(depth.GetAddressOf(), nullptr);
    D3D11_BLEND_DESC blending{};
    if (blend) blend->GetDesc(&blending);
    if (blending.RenderTarget[0].BlendEnable || !depth) return false;

    D3D11_DEPTH_STENCIL_DESC testing{};
    depth->GetDesc(&testing);
    return testing.DepthEnable && (testing.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL || testing.DepthFunc == D3D11_COMPARISON_EQUAL);
}

bool alphaTested(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11PixelShader> pixelShader;
    UINT tested{}, size = sizeof(tested);
    context->PSGetShader(pixelShader.GetAddressOf(), nullptr, nullptr);
    return pixelShader && SUCCEEDED(pixelShader->GetPrivateData(AlphaTestedShader, &size, &tested));
}

// Identifies the mesh range a draw covers. Depth-only passes can bind a position-only vertex
// stream, so the key leaves the vertex buffers out.
std::uint64_t drawKey(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances, std::uint32_t first, std::int32_t baseVertex,
    std::uint32_t firstInstance, bool indexed)
{
    ComPtr<ID3D11Buffer> indices;
    DXGI_FORMAT format{};
    UINT offset{};
    if (indexed) context->IAGetIndexBuffer(indices.GetAddressOf(), &format, &offset);
    std::uint64_t key = 14695981039346656037ull;
    for (const auto value : {reinterpret_cast<std::uint64_t>(indices.Get()), std::uint64_t{offset}, std::uint64_t{count}, std::uint64_t{instances},
        std::uint64_t{first}, static_cast<std::uint64_t>(baseVertex), std::uint64_t{firstInstance}})
        key = (key ^ value) * 1099511628211ull;
    return key;
}

// Asked at every draw, not remembered by address: the game frees buffers and makes others, of either
// usage, at the same addresses.
bool staticBuffer(ID3D11Buffer* buffer)
{
    D3D11_BUFFER_DESC description{};
    buffer->GetDesc(&description);
    return description.Usage != D3D11_USAGE_DYNAMIC;
}

// The object a draw belongs to, whatever its level of detail: the static constant buffers its vertex
// shader reads (the primitive's own; per-draw constants are dynamic) and the instances it draws.
// Instanced foliage keeps its per-instance data in static vertex buffers instead, so instanced draws
// without static constants are told apart by those, per level of detail. Zero when neither applies:
// the draw cannot be told apart from other objects.
std::uint64_t objectKey(ID3D11DeviceContext* context, std::uint32_t instances, std::uint32_t firstInstance)
{
    ComPtr<ID3D11VertexShader> vertexShader;
    context->VSGetShader(vertexShader.GetAddressOf(), nullptr, nullptr);
    UINT slots{}, size = sizeof(slots);
    if (!vertexShader || FAILED(vertexShader->GetPrivateData(ConstantSlots, &size, &slots)) || !slots) return 0;

    std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers{};
    context->VSGetConstantBuffers(0, static_cast<UINT>(std::bit_width(slots)), buffers.data());
    std::uint64_t key{};
    for (UINT slot = 0; slot < buffers.size(); ++slot)
    {
        auto* buffer = buffers[slot];
        if (!buffer) continue;

        if ((slots & (1u << slot)) && staticBuffer(buffer)) key = (key ^ reinterpret_cast<std::uint64_t>(buffer)) * 1099511628211ull;
        buffer->Release();
    }
    if (!key && instances > 1)
    {
        std::array<ID3D11Buffer*, 8> streams{};
        std::array<UINT, 8> strides{}, offsets{};
        context->IAGetVertexBuffers(0, static_cast<UINT>(streams.size()), streams.data(), strides.data(), offsets.data());
        bool dynamic{};
        for (UINT slot = 0; slot < streams.size(); ++slot)
        {
            auto* stream = streams[slot];
            if (!stream) continue;

            dynamic = dynamic || !staticBuffer(stream);
            key = (key ^ reinterpret_cast<std::uint64_t>(stream) ^ (std::uint64_t{offsets[slot]} << 40)) * 1099511628211ull;
            stream->Release();
        }
        if (dynamic) key = 0;
    }
    if (!key) return 0;

    for (const auto value : {std::uint64_t{instances}, std::uint64_t{firstInstance}}) key = (key ^ value) * 1099511628211ull;
    return key;
}

// Offers a draw the view captured to the geometry cache. Objects drawn with velocity move, and are
// never cached: their copies would stay where they were seen.
std::uint64_t cacheKey(std::uint64_t object, std::uint64_t draw) { return (object ^ draw) * 1099511628211ull; }

// Whether the view already drew this draw of the object this frame. Bend places some static meshes,
// such as fallen branches, by per-draw constants, which the object key leaves out: every copy has the
// same key, and the cache would stand one copy in for all of them. Such an object is kept out of the
// cache, as a moving one is.
bool drawnBefore(std::uint64_t object, std::uint64_t draw)
{
    return object && !capture.viewDraws.insert(cacheKey(object, draw)).second;
}

// Whether the cache can stand in for a static draw, so the view need not capture it: it holds the
// draw's geometry.
bool cacheStandsIn(const Gpu& g, std::uint64_t object, std::uint64_t draw)
{
    return offscreenGeometry && object && !g.movers.contains(object) && g.cache->contains(cacheKey(object, draw));
}

void standIn(std::uint64_t object, std::uint64_t draw, bool masked)
{
    ++capture.standInDraws;
    capture.capturedKeys.insert(draw);
    capture.viewObjects.insert(object);
    capture.cacheDraws.push_back({cacheKey(object, draw), object, 0, 0, NoMaterial, masked, false, true});
}

void offerToCache(std::uint64_t object, std::uint64_t draw, UINT first, UINT vertices, UINT slot, bool masked, bool moving)
{
    auto& g = *gpu;
    if (!object)
    {
        ++capture.anonymousDraws;
        return;
    }

    // The view's objects and its movers are noted even without the off-screen geometry, when nothing is
    // offered: the shadow casters of the static objects the view drew are left to its capture.
    capture.viewObjects.insert(object);

    // An object that starts moving, or is drawn twice (drawnBefore), leaves the cache: the trace evicts
    // its entries, and its draws offered earlier in the frame go now.
    if (moving && g.movers.try_emplace(object).second)
    {
        g.newMovers.insert(object);
        std::erase_if(capture.cacheDraws, [&](const GeometryCache::Draw& offered) { return offered.object == object; });
    }

    if (const auto mover = g.movers.find(object); mover != g.movers.end())
    {
        mover->second = g.frames;
        ++capture.movingDraws;
    }
    else if (offscreenGeometry) capture.cacheDraws.push_back({cacheKey(object, draw), object, first, vertices, slot, masked});
}

// Whether the cache can stand in for a static object's shadow casters: it holds the object whole and
// this frame's cached geometry can be placed.
bool cacheCasts(const Gpu& g, std::uint64_t object)
{
    return offscreenGeometry && capture.cameraKnown && !g.cacheReset && !g.movers.contains(object) && g.cache->holdsWhole(object);
}

// Bend's foliage: instanced draws whose vertex shader reads each instance's transform (three rows, the
// translation in w) from a static structured buffer at t0, by an index in the low 24 bits of a stream
// at vertex buffer slot 0 that lists the instances visible this frame.
struct FoliageSource { ComPtr<ID3D11Buffer> transforms, indices; UINT indexOffset; };

std::optional<FoliageSource> foliageSource(ID3D11DeviceContext* context)
{
    FoliageSource source;
    UINT stride{};
    context->IAGetVertexBuffers(0, 1, source.indices.GetAddressOf(), &stride, &source.indexOffset);
    ComPtr<ID3D11ShaderResourceView> view;
    context->VSGetShaderResources(0, 1, view.GetAddressOf());
    if (!source.indices || stride != 4 || !view) return std::nullopt;

    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    D3D11_BUFFER_DESC transforms{}, indices{};
    if (FAILED(resource.As(&source.transforms))) return std::nullopt;
    source.transforms->GetDesc(&transforms);
    source.indices->GetDesc(&indices);
    if (transforms.StructureByteStride != FoliageTransformBytes || transforms.Usage == D3D11_USAGE_DYNAMIC || indices.Usage != D3D11_USAGE_DYNAMIC)
        return std::nullopt;
    return source;
}

// The mesh a foliage draw draws, at its level of detail, whichever instances it draws. An alpha-tested
// mesh's shape is its alpha test's too, and Bend draws the same vertices with many plants' textures
// (one with 35): those textures are part of the mesh, or one plant's copies would stand in for others.
std::uint64_t foliageMesh(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t first, std::int32_t baseVertex, bool masked)
{
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11Buffer> indices;
    DXGI_FORMAT format{};
    UINT indexOffset{};
    context->VSGetShader(vertexShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShader(pixelShader.GetAddressOf(), nullptr, nullptr);
    context->IAGetIndexBuffer(indices.GetAddressOf(), &format, &indexOffset);
    std::array<ID3D11Buffer*, 4> streams{};
    std::array<UINT, 4> strides{}, offsets{};
    context->IAGetVertexBuffers(0, static_cast<UINT>(streams.size()), streams.data(), strides.data(), offsets.data());
    std::uint64_t key = 14695981039346656037ull;
    for (const auto value : {reinterpret_cast<std::uint64_t>(vertexShader.Get()), reinterpret_cast<std::uint64_t>(pixelShader.Get()),
        reinterpret_cast<std::uint64_t>(indices.Get()), std::uint64_t{indexOffset}, std::uint64_t{count}, std::uint64_t{first},
        static_cast<std::uint64_t>(baseVertex), std::uint64_t{masked}})
        key = (key ^ value) * 1099511628211ull;
    // Slot 0 is the per-frame instance list; the others hold the mesh.
    for (std::size_t slot = 0; slot < streams.size(); ++slot)
    {
        if (!streams[slot]) continue;

        if (slot) key = (key ^ reinterpret_cast<std::uint64_t>(streams[slot]) ^ (std::uint64_t{offsets[slot]} << 40)) * 1099511628211ull;
        streams[slot]->Release();
    }

    BakeInput input{};
    UINT inputSize = sizeof(input);
    if (!masked || !pixelShader || FAILED(pixelShader->GetPrivateData(BakeInputData, &inputSize, &input))) return key;

    // The textures the alpha test declares, as its coverage is baked from them.
    std::array<ID3D11ShaderResourceView*, 32> views{};
    const auto declared = static_cast<UINT>(32 - std::countl_zero(input.textures));
    context->PSGetShaderResources(0, declared, views.data());
    for (UINT slot = 0; slot < declared; ++slot)
    {
        if (input.textures & (1u << slot)) key = (key ^ reinterpret_cast<std::uint64_t>(views[slot])) * 1099511628211ull;
        if (views[slot]) views[slot]->Release();
    }

    return key;
}

// Whether the trees' copies can stand in for a foliage draw of this mesh, so the view need not
// capture it: its template resolved to a reference at a frame's first draw (placeFoliage), so its
// trees are placed. The copies keep the wind of their template's frame.
bool copiesStandIn(const Gpu& g, std::uint64_t mesh)
{
    if (!offscreenGeometry || !capture.cameraKnown) return false;

    const auto model = g.foliageTemplates.find(mesh);
    return model != g.foliageTemplates.end() && model->second.placedBy;
}

// Records a foliage draw the view captured: its transforms now and then and, every FoliageLatency-th
// frame, its visible instance list, read back a few frames later to learn which trees exist and with
// which mesh, and once per mesh its first instance as the template the trees are placed from. Templates
// are only taken in those frames: the list read back tells which instance the template shows.
void recordFoliage(ID3D11DeviceContext* context, const FoliageSource& source, std::uint64_t mesh, std::uint32_t instances,
    std::uint32_t firstInstance, UINT firstVertex, UINT vertices, UINT slot, bool masked)
{
    auto& g = *gpu;
    auto& trees = g.foliage[source.transforms.Get()];
    trees.drawnFrame = g.frames;
    D3D11_BUFFER_DESC description{};
    source.transforms->GetDesc(&description);
    if (!trees.source)
    {
        trees.source = source.transforms;
        trees.meshes.resize(description.ByteWidth / FoliageTransformBytes);
        trees.seenIn.resize(trees.meshes.size());
    }
    // The transforms, again now and then: streaming changes them.
    if (!trees.pending && (trees.transforms.empty() || g.frames - trees.loadedFrame > FoliageReloadFrames))
    {
        if (!trees.staging)
        {
            auto staging = description;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(g.device->CreateBuffer(&staging, nullptr, trees.staging.GetAddressOf()), "Creating the foliage transform readback");
        }
        context->CopyResource(trees.staging.Get(), source.transforms.Get());
        trees.pending = true;
    }

    auto& readback = g.foliageReadback;
    if (g.frames % FoliageLatency || readback.pending || capture.foliageIndices + instances > FoliageIndexCapacity) return;

    const D3D11_BOX listed{source.indexOffset + firstInstance * 4, 0, 0, source.indexOffset + (firstInstance + instances) * 4, 1, 1};
    context->CopySubresourceRegion(readback.staging.Get(), 0, capture.foliageIndices * 4, 0, 0, source.indices.Get(), 0, &listed);

    std::uint64_t templateKey{};
    if (!g.foliageTemplates.contains(mesh))
    {
        templateKey = (mesh ^ g.frames) * 1099511628211ull | 1;
        g.foliageTemplates[mesh] = {templateKey, source.transforms.Get(), NoInstance};
        capture.templateDraws.push_back({templateKey, 0, firstVertex, vertices / instances, slot, masked, true});
    }
    capture.foliageDraws.push_back({mesh, source.transforms.Get(), capture.foliageIndices, instances, templateKey});
    capture.foliageIndices += instances;
}

// Runs the game's alpha-test pixel shader, with the textures and constants of the current draw, over
// its texture coordinates into a free coverage slot, once per fade level when it reads a fade.
// Returns the slot.
UINT bake(Gpu& g, ID3D11DeviceContext* context, ID3D11PixelShader* pixelShader, ID3D11VertexShader* bakeShader, std::uint32_t textures,
    bool fades, UINT toggles, UINT transformRow)
{
    // The material is the shader and the textures it declares; other slots can hold anything. Draws
    // look it up every frame, so its key has a place for every slot and is built without allocating,
    // and the samplers and constants are fetched only to bake it.
    std::array<ID3D11ShaderResourceView*, 32> views{};
    std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{};
    std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers{};
    const auto declared = static_cast<UINT>(32 - std::countl_zero(textures));
    context->PSGetShaderResources(0, declared, views.data());
    Gpu::MaterialKey key{pixelShader};
    for (UINT slot = 0; slot < declared; ++slot)
        if (textures & (1u << slot)) key[1 + slot] = views[slot];
    const auto release = [&]
    {
        for (auto* view : views)
            if (view) view->Release();
        for (auto* sampler : samplers)
            if (sampler) sampler->Release();
        for (auto* buffer : buffers)
            if (buffer) buffer->Release();
    };
    if (const auto found = g.materials.find(key); found != g.materials.end())
    {
        release();
        found->second.usedIn = g.frames;
        return found->second.slot;
    }
    context->PSGetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    context->PSGetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.data());
    // A slot covers a whole texture, so its samples would take a small mip, where thin needles blur
    // into an opacity that passes everywhere. Copies of the samplers keep to the finest mip.
    std::array<ComPtr<ID3D11SamplerState>, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> finest;
    std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> finestSamplers{};
    for (std::size_t s = 0; s < samplers.size(); ++s)
    {
        if (!samplers[s]) continue;

        D3D11_SAMPLER_DESC description{};
        samplers[s]->GetDesc(&description);
        description.MipLODBias = 0;
        description.MinLOD = description.MaxLOD = 0;
        g.device->CreateSamplerState(&description, finest[s].GetAddressOf());
        finestSamplers[s] = finest[s] ? finest[s].Get() : samplers[s];
    }
    // When the atlas is full, start again: materials still in view are baked again as they are drawn.
    if (g.materials.size() >= MaterialSlots)
    {
        g.materials.clear();
        g.cacheReset = true;
        static bool reported{};
        if (!std::exchange(reported, true)) upscaler::log("The coverage atlas filled; materials are baked again from the start.");
    }

    // With no slot freed, the materials hold the slots below their count.
    auto slot = static_cast<UINT>(g.materials.size());
    if (!g.freeSlots.empty())
    {
        slot = g.freeSlots.back();
        g.freeSlots.pop_back();
    }
    const BakeConstants constants{{slot % AtlasSlots * SlotSize, slot / AtlasSlots * SlotSize}, {}};
    inOwnState(g, [&]
    {
        const D3D11_VIEWPORT viewport{0, 0, SlotSize * BakeSamples, SlotSize * BakeSamples, 0, 1};
        g.context->ClearDepthStencilView(g.bakeTarget.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        g.context->RSSetViewports(1, &viewport);
        g.context->RSSetState(g.noCull.Get());
        g.context->OMSetRenderTargets(0, nullptr, g.bakeTarget.Get());
        g.context->OMSetDepthStencilState(g.lowestLevel.Get(), 0);
        g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.context->VSSetShader(bakeShader, nullptr, 0);
        g.context->VSSetConstantBuffers(0, 1, g.bakeLevel.GetAddressOf());
        g.context->VSSetShaderResources(0, 1, g.transformRowsView.GetAddressOf());
        g.context->PSSetShader(pixelShader, nullptr, 0);
        g.context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        g.context->PSSetSamplers(0, static_cast<UINT>(finestSamplers.size()), finestSamplers.data());
        g.context->PSSetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.data());
        for (UINT level = 0; level < (fades ? FadeLevels : 1); ++level)
            for (UINT combination = 0; combination < 1u << toggles; ++combination)
            {
                const BakeLevel levelConstants{static_cast<float>(level) / (FadeLevels - 1), static_cast<float>(level * 16) / 255, combination,
                    transformRow};
                g.context->UpdateSubresource(g.bakeLevel.Get(), 0, nullptr, &levelConstants, 0, 0);
                g.context->Draw(3, 0);
            }
        g.context->OMSetRenderTargets(0, nullptr, nullptr);
        g.context->UpdateSubresource(g.bakeConstants.Get(), 0, nullptr, &constants, 0, 0);
        g.context->CSSetShader(g.bakeCopyShader.Get(), nullptr, 0);
        g.context->CSSetConstantBuffers(0, 1, g.bakeConstants.GetAddressOf());
        g.context->CSSetShaderResources(0, 1, g.bakeView.GetAddressOf());
        g.context->CSSetUnorderedAccessViews(0, 1, g.coverageAccess.GetAddressOf(), nullptr);
        g.context->Dispatch(SlotSize / 8, SlotSize / 8, 1);
    });
    Gpu::Material material{slot, g.frames};
    for (auto* object : key)
        if (object) material.references.emplace_back(object);
    release();
    g.materials.emplace(key, std::move(material));
    return slot;
}

// Stream output of an alpha-tested draw: the clip position, the coordinates its alpha test samples
// with and its fade. Made per vertex shader output signature and alpha-test inputs.
ComPtr<ID3D11GeometryShader> coordinateOutput(Gpu& g, ID3D11VertexShader* vertexShader, const BakeInput& input)
{
    UINT size{};
    if (FAILED(vertexShader->GetPrivateData(VertexBytecode, &size, nullptr)) || !size) return nullptr;

    std::vector<std::uint8_t> code(size);
    vertexShader->GetPrivateData(VertexBytecode, &size, code.data());
    ComPtr<ID3DBlob> signature;
    if (FAILED(D3DGetOutputSignatureBlob(code.data(), code.size(), signature.GetAddressOf()))) return nullptr;

    std::string key(static_cast<const char*>(signature->GetBufferPointer()), signature->GetBufferSize());
    key.append(reinterpret_cast<const char*>(&input), sizeof(input));
    auto& cached = g.coordinateOutput[key];
    if (cached) return cached;

    // Stream output counts components from the element's first, wherever it sits in its register.
    // Without a fade the baked coverage is all or nothing, and the fade is skipped.
    const bool fades = *input.fadeSemantic;
    const D3D11_SO_DECLARATION_ENTRY entries[]{{0, "SV_Position", 0, 0, 4, 0},
        {0, input.semantic, input.semanticIndex, static_cast<BYTE>(input.component), 2, 0},
        {0, fades ? input.fadeSemantic : nullptr, fades ? input.fadeSemanticIndex : 0, static_cast<BYTE>(fades ? input.fadeComponent : 0), 1, 0}};
    const UINT stride = MaskedVertexBytes;
    g.device->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), entries, 3, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
        cached.GetAddressOf());
    return cached;
}

// The bake vertex shader of an alpha-tested pixel shader, compiled on first use and kept on it. Its
// outputs must land where the pixel shader reads its inputs; a material whose do not is not baked.
ComPtr<ID3D11VertexShader> bakeVertexShader(Gpu& g, ID3D11PixelShader* pixelShader)
{
    ComPtr<ID3D11VertexShader> shader;
    UINT size = sizeof(ID3D11VertexShader*);
    if (SUCCEEDED(pixelShader->GetPrivateData(BakeShader, &size, shader.GetAddressOf()))) return shader;

    size = 0;
    if (FAILED(pixelShader->GetPrivateData(BakeShaderSource, &size, nullptr)) || !size) return nullptr;
    std::string source(size, '\0');
    pixelShader->GetPrivateData(BakeShaderSource, &size, source.data());
    pixelShader->SetPrivateData(BakeShaderSource, 0, nullptr);
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "ray-tracing-bake", nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf()))
        || FAILED(g.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf())))
        return nullptr;
    pixelShader->SetPrivateDataInterface(BakeShader, shader.Get());
    return shader;
}

// Captures an alpha-tested draw of the depth prepass, where Bend alpha-tests: streams its vertices
// with texture coordinates and bakes its material's coverage the first time it is seen.
bool captureAlphaTested(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances, std::uint32_t first,
    std::int32_t baseVertex, std::uint32_t firstInstance, bool indexed)
{
    auto& g = *gpu;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11VertexShader> vertexShader, bakeShader;
    ComPtr<ID3D11GeometryShader> geometryShader;
    ComPtr<ID3D11HullShader> hullShader;
    BakeInput input{};
    UINT inputSize = sizeof(input);
    context->PSGetShader(pixelShader.GetAddressOf(), nullptr, nullptr);
    context->VSGetShader(vertexShader.GetAddressOf(), nullptr, nullptr);
    context->GSGetShader(geometryShader.GetAddressOf(), nullptr, nullptr);
    context->HSGetShader(hullShader.GetAddressOf(), nullptr, nullptr);
    const auto vertices = streamedVertices(context, count, instances);
    if (!pixelShader || !vertexShader || geometryShader || hullShader || !vertices
        || FAILED(pixelShader->GetPrivateData(BakeInputData, &inputSize, &input)) || !(bakeShader = bakeVertexShader(g, pixelShader.Get())))
    {
        ++capture.unbakedDraws;
        return false;
    }
    auto& pair = g.coordinateOutputByShaders[{vertexShader.Get(), pixelShader.Get()}];
    if (!pair.vertexShader) pair = {vertexShader, pixelShader, coordinateOutput(g, vertexShader.Get(), input)};
    pair.usedIn = g.frames;
    const auto& streamOutput = pair.output;
    if (!streamOutput)
    {
        ++capture.unbakedDraws;
        return false;
    }
    if (vertices > MaskedCapacity - capture.maskedVertices || vertices > MaskedEnd - capture.opaqueVertices - capture.maskedVertices
        || capture.materialDraws.size() >= MaxMaterialDraws || (input.transformed && capture.transformDraws.size() >= MaxTransformDraws))
    {
        ++capture.overCapacityDraws;
        return false;
    }

    // A material tiling its texture is baked one tile; the draw's own scale and offset take its
    // coordinates there.
    TransformDraw transform{capture.maskedVertices, capture.maskedVertices + static_cast<UINT>(vertices),
        static_cast<std::uint32_t>(capture.transformDraws.size()), 0, {}};
    if (input.transformed)
    {
        const TransformValue values[]{input.transform.scale[0], input.transform.scale[1], input.transform.offset[0], input.transform.offset[1]};
        for (UINT k = 0; k < 4; ++k)
        {
            const auto& value = values[k];
            if (!value.constant)
            {
                transform.literals[k] = value.literal;
                continue;
            }

            ComPtr<ID3D11Buffer> constants;
            D3D11_BUFFER_DESC description{};
            if (value.buffer < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) context->PSGetConstantBuffers(value.buffer, 1, constants.GetAddressOf());
            if (constants) constants->GetDesc(&description);
            if (description.ByteWidth < (value.reg + 1) * 16)
            {
                ++capture.unbakedDraws;
                return false;
            }

            const D3D11_BOX row{value.reg * 16, 0, 0, (value.reg + 1) * 16, 1, 1};
            context->CopySubresourceRegion(g.transformRows.Get(), 0, (transform.row * 4 + k) * 16, 0, 0, constants.Get(), 0, &row);
            transform.sources |= 1u << k | (value.component & 3) << (8 + 2 * k);
        }
    }
    const auto slot = bake(g, context, pixelShader.Get(), bakeShader.Get(), input.textures, *input.fadeSemantic, input.toggles, transform.row);
    UINT offset = capture.maskedVertices * MaskedVertexBytes;
    ID3D11Buffer* target = g.maskedStreamed.Get();
    context->GSSetShader(streamOutput.Get(), nullptr, 0);
    context->SOSetTargets(1, &target, &offset);
    if (indexed) context->DrawIndexedInstanced(count, instances, first, baseVertex, firstInstance);
    else context->DrawInstanced(count, instances, first, firstInstance);
    context->SOSetTargets(0, nullptr, nullptr);
    context->GSSetShader(nullptr, nullptr, 0);
    capture.materialDraws.push_back({capture.maskedVertices, slot});
    if (input.transformed) capture.transformDraws.push_back(transform);
    capture.maskedVertices += static_cast<UINT>(vertices);
    return true;
}

// Repeats an opaque draw with stream output into the opaque range. Returns whether it was captured.
bool captureOpaque(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances, std::uint32_t first, std::int32_t baseVertex,
    std::uint32_t firstInstance, bool indexed)
{
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11GeometryShader> streamOutput, geometryShader;
    ComPtr<ID3D11HullShader> hullShader;
    UINT size = sizeof(ID3D11GeometryShader*);
    context->VSGetShader(vertexShader.GetAddressOf(), nullptr, nullptr);
    if (!vertexShader || FAILED(vertexShader->GetPrivateData(StreamOutputShader, &size, streamOutput.GetAddressOf())))
    {
        ++capture.unknownShaderDraws;
        return false;
    }

    // Tessellation and geometry shaders move vertices after the vertex shader.
    context->GSGetShader(geometryShader.GetAddressOf(), nullptr, nullptr);
    context->HSGetShader(hullShader.GetAddressOf(), nullptr, nullptr);
    const auto vertices = streamedVertices(context, count, instances);
    if (geometryShader || hullShader || !vertices)
    {
        ++capture.unsupportedDraws;
        return false;
    }
    if (vertices > MaskedEnd - capture.opaqueVertices - capture.maskedVertices)
    {
        ++capture.overCapacityDraws;
        return false;
    }

    UINT offset = capture.opaqueVertices * VertexBytes;
    ID3D11Buffer* target = gpu->streamed.Get();
    context->GSSetShader(streamOutput.Get(), nullptr, 0);
    context->SOSetTargets(1, &target, &offset);
    if (indexed) context->DrawIndexedInstanced(count, instances, first, baseVertex, firstInstance);
    else context->DrawInstanced(count, instances, first, firstInstance);
    context->SOSetTargets(0, nullptr, nullptr);
    context->GSSetShader(nullptr, nullptr, 0);
    capture.opaqueVertices += static_cast<UINT>(vertices);
    return true;
}

// Bend renders the sun's shadow into an atlas of three square maps side by side. The first, the far
// map, covers the whole shadow distance; the other two, the near cascades, hold the casters near the
// view, which the far map leaves out. Returns the map a draw renders into, if any, and its viewport.
// Draws into a map write depth; full-screen passes would not test it.
std::optional<UINT> shadowMapDraw(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, D3D11_VIEWPORT& viewport)
{
    ComPtr<ID3D11Resource> target;
    view->GetResource(target.GetAddressOf());
    UINT viewports = 1;
    context->RSGetViewports(&viewports, &viewport);
    const auto side = static_cast<float>(gpu->atlasHeight);
    const auto map = static_cast<UINT>(viewport.TopLeftX / std::max(side, 1.0f));
    if (target.Get() != gpu->shadowAtlas.Get() || map > 2 || viewport.TopLeftX != map * side || viewport.TopLeftY != 0
        || viewport.Width != side || viewport.Height != side)
        return std::nullopt;

    ComPtr<ID3D11DepthStencilState> depth;
    context->OMGetDepthStencilState(depth.GetAddressOf(), nullptr);
    D3D11_DEPTH_STENCIL_DESC testing{};
    if (depth) depth->GetDesc(&testing);
    if (!testing.DepthEnable || testing.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL || testing.DepthFunc == D3D11_COMPARISON_ALWAYS)
        return std::nullopt;
    return map;
}

// Captures a shadow caster: geometry off screen or hidden from the view still shadows it. Casters
// follow the view's geometry, which the base pass has finished drawing. Most casters repeat a draw
// already captured, on screen or in another of the maps (those near the view are drawn into more
// than one): those are skipped. So are the casters of a static object the view drew, which its
// capture holds whole at the view's level of detail, and of one the cache holds whole, which is
// traced from there. Moving objects are not told apart (their own constants are dynamic, and every
// one of them has the same object key), so their casters are captured: another one may be off
// screen. The views of what sun rays see beyond the view keep every caster.
void captureShadowCaster(ID3D11DeviceContext* context, UINT map, const D3D11_VIEWPORT& viewport, std::uint32_t count,
    std::uint32_t instances, std::uint32_t first, std::int32_t baseVertex, std::uint32_t firstInstance, bool indexed)
{
    const auto key = drawKey(context, count, instances, first, baseVertex, firstInstance, indexed);
    if (capture.capturedKeys.contains(key))
    {
        ++capture.repeatedCasterDraws;
        return;
    }

    const bool beyondView = selected == View::ShadowCasters || selected == View::OffscreenShadows;
    const auto object = beyondView ? 0 : objectKey(context, instances, firstInstance);
    if (object && !gpu->movers.contains(object) && capture.viewObjects.contains(object))
    {
        ++capture.casterDrawsInView;
        capture.capturedKeys.insert(key);
        return;
    }
    if (object && cacheCasts(*gpu, object))
    {
        ++capture.casterDrawsInCache;
        capture.casting.insert(object);
        capture.capturedKeys.insert(key);
        return;
    }

    // Like the view's foliage, some casters narrow the depth range: their clip z is not the map's depth.
    const auto& last = capture.casterRuns.empty() ? Capture::CasterRun{} : capture.casterRuns.back();
    if (capture.casterRuns.empty() || last.map != map || last.minDepth != viewport.MinDepth || last.maxDepth != viewport.MaxDepth)
    {
        if (capture.casterRuns.size() == MaxCasterRuns)
        {
            ++capture.unsupportedDraws;
            return;
        }
        capture.casterRuns.push_back({capture.opaqueVertices, capture.maskedVertices, map, viewport.MinDepth, viewport.MaxDepth});
    }
    const bool captured = alphaTested(context) ? captureAlphaTested(context, count, instances, first, baseVertex, firstInstance, indexed)
                                               : captureOpaque(context, count, instances, first, baseVertex, firstInstance, indexed);
    if (!captured) return;

    ++capture.casterDraws;
    capture.capturedKeys.insert(key);
}

// The sun shadow resolve's constants give the sun direction and its target the game's shadow. The
// constants are read back a few frames later, so the CPU never waits for the GPU. Returns whether
// this is a resolve draw.
bool shadowResolve(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11PixelShader> pixelShader;
    UINT slot{}, size = sizeof(slot);
    context->PSGetShader(pixelShader.GetAddressOf(), nullptr, nullptr);
    if (!pixelShader || FAILED(pixelShader->GetPrivateData(ShadowResolveShader, &size, &slot))) return false;

    ++capture.shadowResolveDraws;
    capture.shadowPending = true;
    // The shadow map atlas it samples: three square maps side by side. A downsampled copy in the same
    // layout is bound beside it.
    std::array<ID3D11ShaderResourceView*, 8> views{};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    UINT largest{};
    for (auto* view : views)
    {
        if (!view) continue;

        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11Texture2D> texture;
        view->GetResource(resource.GetAddressOf());
        D3D11_TEXTURE2D_DESC description{};
        if (SUCCEEDED(resource.As(&texture))) texture->GetDesc(&description);
        if (description.Height && description.Width == description.Height * 3 && description.Height >= largest)
        {
            largest = description.Height;
            gpu->shadowAtlas = resource;
            gpu->atlasWidth = description.Width;
            gpu->atlasHeight = description.Height;
        }
        view->Release();
    }
    if (!capture.shadow)
    {
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11Resource> resource;
        context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        D3D11_RENDER_TARGET_VIEW_DESC description{};
        if (target)
        {
            target->GetDesc(&description);
            target->GetResource(resource.GetAddressOf());
            resource.As(&capture.shadow);
            capture.shadowFormat = description.Format;
        }
    }
    if (std::exchange(capture.sunCopied, true)) return true;

    ComPtr<ID3D11Buffer> constants;
    D3D11_BUFFER_DESC description{}, stagingDescription{};
    context->PSGetConstantBuffers(slot, 1, constants.GetAddressOf());
    if (!constants) return true;

    constants->GetDesc(&description);
    auto& staging = gpu->sunStaging[gpu->sunNext];
    if (staging) staging->GetDesc(&stagingDescription);
    if (description.ByteWidth < (SunRow + 1) * 16) return true;

    if (stagingDescription.ByteWidth != description.ByteWidth)
    {
        stagingDescription = {description.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
        check(gpu->device->CreateBuffer(&stagingDescription, nullptr, staging.ReleaseAndGetAddressOf()), "Creating the sun readback buffer");
    }
    context->CopyResource(staging.Get(), constants.Get());
    if (description.ByteWidth >= (LightRows + LightRowCount) * 16)
    {
        const D3D11_BOX rows{LightRows * 16, 0, 0, (LightRows + LightRowCount) * 16, 1, 1};
        context->CopySubresourceRegion(gpu->lightSpace.Get(), 0, 0, 0, 0, constants.Get(), 0, &rows);
    }
    gpu->sunPending[gpu->sunNext] = true;
    gpu->sunNext = (gpu->sunNext + 1) % FramesInFlight;
    return true;
}

// Draws the sun shadow resolve without Bend's soft tree shadow. It comes from a light-space map of
// the canopies, in two cascades, the only array texture every variant reads: zeros there leave the
// sun's visibility to the shadow maps. The game's binding is put back, since the engine assumes it
// is still bound. Returns whether it drew.
bool resolveWithoutCanopy(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances, std::uint32_t first,
    std::int32_t baseVertex, std::uint32_t firstInstance, bool indexed)
{
    std::array<ID3D11ShaderResourceView*, 8> views{};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    std::optional<UINT> canopy;
    for (UINT slot = 0; slot < views.size(); ++slot)
    {
        if (!views[slot]) continue;

        D3D11_SHADER_RESOURCE_VIEW_DESC description{};
        views[slot]->GetDesc(&description);
        if (description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) canopy = slot;
    }
    if (canopy)
    {
        context->PSSetShaderResources(*canopy, 1, gpu->noCanopy.GetAddressOf());
        if (indexed) context->DrawIndexedInstanced(count, instances, first, baseVertex, firstInstance);
        else context->DrawInstanced(count, instances, first, firstInstance);
        context->PSSetShaderResources(*canopy, 1, &views[*canopy]);
    }
    for (auto* view : views)
        if (view) view->Release();
    return canopy.has_value();
}

// The shadow target is pooled and reused later in the frame, so the comparison and the traced
// shadows keep a copy taken as lighting reads it.
void copyShadow(Gpu& g, Capture& frame)
{
    frame.shadowPending = false;
    if (!frame.shadow) return;

    D3D11_TEXTURE2D_DESC description{}, copied{};
    frame.shadow->GetDesc(&description);
    if (g.shadowCopy) g.shadowCopy->GetDesc(&copied);
    if (!g.shadowCopy || copied.Width != description.Width || copied.Height != description.Height || copied.Format != description.Format)
    {
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = description.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = frame.shadowFormat;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        check(g.device->CreateTexture2D(&description, nullptr, g.shadowCopy.ReleaseAndGetAddressOf()), "Creating the sun shadow copy");
        check(g.device->CreateShaderResourceView(g.shadowCopy.Get(), &view, g.shadowView.ReleaseAndGetAddressOf()), "Creating the sun shadow view");
        upscaler::log("Sun shadow target: " + std::to_string(description.Width) + "x" + std::to_string(description.Height) + ", format "
            + std::to_string(frame.shadowFormat) + ".");
    }
    g.context->CopyResource(g.shadowCopy.Get(), frame.shadow.Get());
    frame.shadowCopied = true;
}

// Whether the draw reads the resolved sun shadow. The target is final only when lighting reads it:
// other passes write it after the resolves.
bool readsShadow(ID3D11DeviceContext* context)
{
    std::array<ID3D11ShaderResourceView*, 32> views{};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    bool reads{};
    for (auto* view : views)
    {
        if (!view) continue;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.GetAddressOf());
        reads = reads || resource.Get() == capture.shadow.Get();
        view->Release();
    }
    return reads;
}

// As lighting first reads the sun shadow: keeps a copy of the game's shadow and the lighting's
// constants the bounced light needs, traces the frame and, with the feature on, adds the traced
// shadows to the target it is about to read.
void shadowsResolved(Gpu& g)
{
    copyShadow(g, capture);
    readLighting(g, capture);
    if (!capture.traced) capture.traced = trace(g, capture);
    if (capture.traced && capture.shadowCopied && g.sunFound && activeFeatures().sunShadows) inject(g, capture);
}

// Whether the draw's pixel shader reads the resource.
bool readsResource(ID3D11DeviceContext* context, ID3D11Resource* resource)
{
    std::array<ID3D11ShaderResourceView*, 32> views{};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    bool reads{};
    for (auto* view : views)
    {
        if (!view) continue;
        ComPtr<ID3D11Resource> bound;
        view->GetResource(bound.GetAddressOf());
        reads = reads || bound.Get() == resource;
        view->Release();
    }
    return reads;
}

// Combines the traced ambient occlusion with the game's own (AmbientPack) and runs the game's blur
// over it, keeping a copy of the game's. False where the game's target is not the full-resolution
// R32G32_UINT the trace matches.
bool filterAmbient(Gpu& g, Capture& frame)
{
    auto& a = g.ambient;
    ComPtr<ID3D11Texture2D> target, normals;
    D3D11_TEXTURE2D_DESC description{}, normal{}, current{};
    if (FAILED(frame.ambient.As(&target)) || FAILED(frame.normals.As(&normals)) || !a.blur) return false;

    target->GetDesc(&description);
    normals->GetDesc(&normal);
    if (description.Format != DXGI_FORMAT_R32G32_UINT || description.Width != normal.Width || description.Height != normal.Height)
    {
        static bool reported{};
        if (!std::exchange(reported, true))
            upscaler::log("The game's ambient occlusion is " + std::to_string(description.Width) + "x" + std::to_string(description.Height)
                + ", format " + std::to_string(description.Format) + ", not R32G32_UINT at the G-buffer's size; the traced occlusion is not used.");
        return false;
    }

    if (a.game) a.game->GetDesc(&current);
    if (!a.game || current.Width != description.Width || current.Height != description.Height)
    {
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        description.CPUAccessFlags = description.MiscFlags = 0;
        for (auto** texture : {a.game.ReleaseAndGetAddressOf(), a.packed.ReleaseAndGetAddressOf(), a.filtered.ReleaseAndGetAddressOf()})
            check(g.device->CreateTexture2D(&description, nullptr, texture), "Creating an ambient occlusion target");
        check(g.device->CreateShaderResourceView(a.game.Get(), nullptr, a.gameView.ReleaseAndGetAddressOf()), "Creating an ambient occlusion view");
        check(g.device->CreateShaderResourceView(a.packed.Get(), nullptr, a.packedView.ReleaseAndGetAddressOf()), "Creating an ambient occlusion view");
        check(g.device->CreateShaderResourceView(a.filtered.Get(), nullptr, a.filteredView.ReleaseAndGetAddressOf()),
            "Creating an ambient occlusion view");
        check(g.device->CreateUnorderedAccessView(a.packed.Get(), nullptr, a.packedAccess.ReleaseAndGetAddressOf()), "Creating an ambient occlusion view");
        check(g.device->CreateUnorderedAccessView(a.filtered.Get(), nullptr, a.filteredAccess.ReleaseAndGetAddressOf()),
            "Creating an ambient occlusion view");
    }
    const RegionConstants region{{frame.rect[0], frame.rect[1]}, {a.width, a.height}};
    const UINT groupsX = (description.Width + 7) / 8, groupsY = (description.Height + 7) / 8;
    inOwnState(g, [&]
    {
        g.context->CopyResource(a.game.Get(), target.Get());
        g.context->UpdateSubresource(g.regionConstants.Get(), 0, nullptr, &region, 0, 0);
        ID3D11ShaderResourceView* inputs[]{a.tracedView.Get(), a.gameView.Get()};
        g.context->CSSetShader(a.packShader.Get(), nullptr, 0);
        g.context->CSSetConstantBuffers(0, 1, g.regionConstants.GetAddressOf());
        g.context->CSSetShaderResources(0, 2, inputs);
        g.context->CSSetUnorderedAccessViews(0, 1, a.packedAccess.GetAddressOf(), nullptr);
        g.context->Dispatch(groupsX, groupsY, 1);

        ID3D11UnorderedAccessView* none{};
        g.context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        g.context->CSSetShader(a.blur.Get(), nullptr, 0);
        g.context->CSSetShaderResources(0, 1, a.packedView.GetAddressOf());
        g.context->CSSetUnorderedAccessViews(0, 1, a.filteredAccess.GetAddressOf(), nullptr);
        g.context->Dispatch(groupsX, groupsY, 1);
    });
    frame.ambientFiltered = true;
    return true;
}

// As lighting first reads the game's ambient occlusion: traces the frame if the sun shadows have not
// and, with the feature on, puts the traced occlusion combined with the game's in its place.
void ambientRead(Gpu& g)
{
    capture.ambientRead = true;
    if (!capture.traced) capture.traced = trace(g, capture);
    if (!capture.traced || !g.ambient.tracing) return;

    g.timer->begin(g.context, Pass::AmbientFilter);
    if (filterAmbient(g, capture) && activeFeatures().ambientOcclusion)
    {
        inOwnState(g, [&] { g.context->CopyResource(capture.ambient.Get(), g.ambient.filtered.Get()); });
        capture.ambientInjected = true;
    }
    g.timer->end(g.context, Pass::AmbientFilter);
}

// Takes the newest sun direction the GPU has finished copying, oldest copy first.
void readSun(Gpu& g)
{
    for (UINT i = 0; i < FramesInFlight; ++i)
    {
        const auto index = (g.sunNext + i) % FramesInFlight;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!g.sunPending[index]) continue;
        if (g.context->Map(g.sunStaging[index].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) break;

        const auto* row = static_cast<const float*>(mapped.pData) + SunRow * 4;
        const auto length = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        // A sun or moon that casts shadows is above the horizon, so the direction toward it points up.
        const auto scale = (row[2] < 0 ? -1.0f : 1.0f) / length;
        if (std::isfinite(scale))
        {
            for (int k = 0; k < 3; ++k) g.sun[k] = row[k] * scale;
            g.sunFound = true;
        }
        g.context->Unmap(g.sunStaging[index].Get(), 0);
        g.sunPending[index] = false;
    }
}

// Adds this frame's costs to the averages, which follow about the last half second. GPU times are of
// the newest frames each device has finished; frames that traced nothing count as costing nothing.
void sampleCosts(const Capture& frame, bool traced, float frameMilliseconds)
{
    Costs sample{};
    sample.frame = frameMilliseconds;
    sample.captureCpu = millisecondsSince(frame.captureTicks);
    sample.traceCpu = millisecondsSince(std::exchange(traceTicks, 0));
    sample.gpuWaitCpu = millisecondsSince(std::exchange(gpuWaitTicks, 0));
    sample.foliageCpu = millisecondsSince(std::exchange(foliageTicks, 0));
    sample.instancesCpu = millisecondsSince(std::exchange(instancesTicks, 0));
    sample.flushCpu = millisecondsSince(std::exchange(flushTicks, 0));
    sample.submitCpu = millisecondsSince(std::exchange(submitTicks, 0));
    sample.prepareCpu = millisecondsSince(std::exchange(prepareTicks, 0));
    if (gpu)
    {
        if (const auto passes = gpu->timer->endFrame(gpu->context)) gpu->passes = *passes;
    }
    if (gpu && traced)
    {
        const auto& t = gpu->timings;
        const auto pass = [&](Pass pass) { return gpu->passes[static_cast<std::size_t>(pass)]; };
        sample.prepare = pass(Pass::Prepare);
        sample.copy = t.copy;
        sample.cacheBuild = t.cache;
        sample.templateBuild = t.templates;
        sample.viewBuild = t.view;
        sample.casterBuild = t.casters;
        sample.topBuild = t.top;
        sample.sunRays = t.trace;
        sample.sunFilter = pass(Pass::SunFilter);
        sample.sunInject = pass(Pass::SunInject);
        sample.ambientRays = gpu->ambient.milliseconds;
        sample.ambientFilter = pass(Pass::AmbientFilter);
        sample.bounceRays = gpu->bounce.milliseconds;
        sample.bounceFilter = pass(Pass::BounceFilter);
        sample.bounceAdd = pass(Pass::BounceAdd);
        sample.debugView = pass(Pass::DebugView);
        // Direct3D 11 waits for all of Direct3D 12's work; what is left is the hand-over.
        const auto direct3d12 = t.copy + t.cache + t.templates + t.view + t.casters + t.top + t.trace + sample.ambientRays + sample.bounceRays;
        sample.sync = std::max(0.0f, pass(Pass::Wait) - direct3d12);
        if (frame.tracedLights)
        {
            sample.lightRays = gpu->lights.milliseconds;
            sample.lightSync = std::max(0.0f, pass(Pass::LightsWait) - sample.lightRays);
            sample.lightFilter = pass(Pass::LightsFilter);
            sample.lightAdd = pass(Pass::LightsAdd);
        }
    }

    const auto weight = 1 - std::exp(-frameMilliseconds / 500);
    for (auto member : {&Costs::frame, &Costs::captureCpu, &Costs::traceCpu, &Costs::gpuWaitCpu, &Costs::foliageCpu, &Costs::instancesCpu,
        &Costs::flushCpu, &Costs::submitCpu, &Costs::prepareCpu, &Costs::prepare, &Costs::copy, &Costs::viewBuild,
        &Costs::casterBuild, &Costs::cacheBuild, &Costs::templateBuild, &Costs::topBuild, &Costs::sync, &Costs::sunRays, &Costs::sunFilter, &Costs::sunInject,
        &Costs::ambientRays, &Costs::ambientFilter, &Costs::bounceRays, &Costs::bounceFilter, &Costs::bounceAdd, &Costs::lightRays, &Costs::lightSync,
        &Costs::lightFilter, &Costs::lightAdd, &Costs::debugView})
        costs.*member += (sample.*member - costs.*member) * weight;
}

void applySetting(const Measurement::Setting& setting)
{
    sunShadows = setting.features.sunShadows;
    ambientOcclusion = setting.features.ambientOcclusion;
    globalIllumination = setting.features.globalIllumination;
    lightShadows = setting.features.lightShadows;
    softLightShadows = setting.features.softLightShadows;
    lightBounce = setting.features.lightBounce;
    softSunShadows = setting.features.softSunShadows;
    selected = setting.view;
    bypassed = setting.paused;
}

// The steps from what is on now: each leaves out one more feature, which leaves the scene built with
// nothing traced, then the scene too: the A/B key's pause stops all of the work.
void startMeasurement()
{
    auto& m = measurement;
    m = {};
    if (selected == View::Off && !sunShadows && !ambientOcclusion && !globalIllumination && !lightShadows)
    {
        upscaler::log("Nothing to measure: no ray-traced feature or view is on.");
        return;
    }

    m.features = {sunShadows, ambientOcclusion, halfAmbientRays, replaceCanopyShadows, offscreenGeometry, globalIllumination, lightShadows,
        softLightShadows, lightBounce, softSunShadows};
    m.view = selected;
    m.bypassed = bypassed;
    Measurement::Setting setting{"", m.features, m.view, false};
    m.settings.push_back(setting);
    const auto leaveOut = [&](bool on, const char* removed, auto change)
    {
        if (!on) return;

        change(setting);
        setting.removed = removed;
        m.settings.push_back(setting);
    };
    leaveOut(m.view != View::Off, "Debug view", [](auto& s) { s.view = View::Off; });
    leaveOut(m.features.lightShadows && m.features.lightBounce, "Light bounce", [](auto& s) { s.features.lightBounce = false; });
    leaveOut(m.features.lightShadows && m.features.softLightShadows, "Soft light shadows", [](auto& s) { s.features.softLightShadows = false; });
    leaveOut(m.features.lightShadows, "Light shadows", [](auto& s) { s.features.lightShadows = false; });
    leaveOut(m.features.globalIllumination, "Global illumination", [](auto& s) { s.features.globalIllumination = false; });
    leaveOut(m.features.ambientOcclusion, "Ambient occlusion", [](auto& s) { s.features.ambientOcclusion = false; });
    leaveOut(m.features.sunShadows && m.features.softSunShadows, "Soft sun shadows", [](auto& s) { s.features.softSunShadows = false; });
    leaveOut(m.features.sunShadows, "Sun shadows", [](auto& s) { s.features.sunShadows = false; });
    leaveOut(true, "Scene capture and build", [](auto& s) { s.paused = true; });
    m.running = measuring = true;
    m.stepStart = ticksNow();
    applySetting(m.settings.front());
    upscaler::log("Measuring the ray tracer's real cost: " + std::to_string(m.settings.size()) + " steps.");
}

void finishMeasurement()
{
    auto& m = measurement;
    m.running = measuring = false;
    m.finishedAt = GetTickCount64();
    applySetting({"", m.features, m.view, m.bypassed});
    std::string line = "Ray tracing real cost (frame time without each, ms):";
    for (std::size_t i = 0; i < m.results.size(); ++i)
    {
        char text[160]{};
        const auto& step = m.results[i];
        if (i == 0) std::snprintf(text, sizeof(text), " everything on %.2f;", step.frame);
        else std::snprintf(text, sizeof(text), " %s %.2f (%+.2f);", step.removed.c_str(), step.frame, m.results[i - 1].frame - step.frame);
        line += text;
    }
    if (m.results.size() > 1)
    {
        char total[64]{};
        std::snprintf(total, sizeof(total), " total %.2f ms.", m.results.front().frame - m.results.back().frame);
        line += total;
    }
    upscaler::log(line);
}

// One present into the measurement: each step settles, then its frames are timed.
void measure(LONGLONG now)
{
    auto& m = measurement;
    if (measurementRequested.exchange(false) && !m.running) startMeasurement();
    if (!m.running) return;

    if (millisecondsSince(now - m.stepStart) < SettleSeconds * 1000) return;
    if (!m.timingStart)
    {
        m.timingStart = now;
        m.timedFrames = 0;
        return;
    }
    ++m.timedFrames;
    const auto timed = millisecondsSince(now - m.timingStart);
    if (timed < TimedSeconds * 1000) return;

    m.results.push_back({m.settings[m.step].removed, timed / static_cast<float>(m.timedFrames)});
    m.timingStart = 0;
    m.stepStart = now;
    if (++m.step == m.settings.size()) return finishMeasurement();

    applySetting(m.settings[m.step]);
}

void publish(const Capture& frame)
{
    std::lock_guard lock(statusMutex);
    published.tracedFrames = tracedFrames;
    published.basePassDraws = frame.basePassDraws;
    published.capturedDraws = frame.capturedDraws;
    published.maskedDraws = frame.maskedDraws;
    published.unbakedDraws = frame.unbakedDraws;
    published.bakedMaterials = gpu ? static_cast<unsigned>(gpu->materials.size()) : 0;
    published.backgroundDraws = frame.backgroundDraws;
    published.unknownShaderDraws = frame.unknownShaderDraws;
    published.unsupportedDraws = frame.unsupportedDraws;
    published.overCapacityDraws = frame.overCapacityDraws;
    published.casterDraws = frame.casterDraws;
    published.casterDrawsInView = frame.casterDrawsInView;
    published.casterDrawsInCache = frame.casterDrawsInCache;
    published.cachedDraws = gpu ? static_cast<unsigned>(gpu->cache->size()) : 0;
    published.tracedCachedDraws = gpu ? static_cast<unsigned>(gpu->cache->traced()) : 0;
    published.foliageInstances = gpu ? gpu->foliageInstances : 0;
    published.triangles = (frame.opaqueVertices + frame.maskedVertices) / 3;
    published.capacity = Capacity / 3;
    // Shadow casters follow the view's geometry in both ranges.
    const auto casterOpaqueStart = frame.casterRuns.empty() ? frame.opaqueVertices : frame.casterRuns.front().opaqueStart;
    const auto casterMaskedStart = frame.casterRuns.empty() ? frame.maskedVertices : frame.casterRuns.front().maskedStart;
    published.viewTriangles = (casterOpaqueStart + casterMaskedStart) / 3;
    published.casterTriangles = published.triangles - published.viewTriangles;
    published.newCacheEntries = gpu ? static_cast<unsigned>(gpu->cache->building().first + gpu->templates->building().first) : 0;
    published.shadowResolveDraws = frame.shadowResolveDraws;
    published.shadowsInjected = frame.shadowsInjected;
    published.ambientInjected = frame.ambientInjected;
    published.bounceAdded = frame.bounceAdded;
    published.tracedLights = frame.tracedLights;
    published.gameLights = frame.gameLights;
    if (gpu)
    {
        published.sunFound = gpu->sunFound;
        std::copy(std::begin(gpu->sun), std::end(gpu->sun), published.sun);
    }
}

// Publishes what any frame has: the costs and the measurement, traced or not.
void publishCosts()
{
    std::lock_guard lock(statusMutex);
    published.costs = costs;
    auto& m = published.measurement;
    m.running = measurement.running;
    m.step = static_cast<unsigned>(measurement.step);
    m.stepCount = static_cast<unsigned>(measurement.settings.size());
    if (!measurement.running && m.finishedAt != measurement.finishedAt)
    {
        m.steps = measurement.results;
        m.finishedAt = measurement.finishedAt;
    }
}

// The costs as lines of text: a heading, then one line each for the frame, every feature, the CPU, the
// work behind the costs and the real cost.
std::vector<std::string> costLines(const Status& state)
{
    const auto& c = state.costs;
    std::vector<std::string> lines{"Days Gone DLSS ray tracing costs in milliseconds, averaged over about half a second. GPU times are timed "
        "passes; the real cost also includes the repeated draws inside the game's passes."};
    lines.push_back(std::format("Frame {:.2f} ms ({:.1f} fps); ray tracing timed GPU {:.2f} ms, CPU {:.2f} ms", c.frame,
        c.frame > 0 ? 1000 / c.frame : 0, c.gpu(), c.cpu()));
    lines.push_back(std::format("Scene {:.2f}: placing vertices {:.2f}, copy {:.2f}, view BLAS {:.2f}, casters BLAS {:.2f}, cache builds {:.2f}, "
        "foliage templates {:.2f}, TLAS {:.2f}, Direct3D 12 hand-over {:.2f}", c.scene(), c.prepare, c.copy, c.viewBuild, c.casterBuild,
        c.cacheBuild, c.templateBuild, c.topBuild, c.sync));
    lines.push_back(std::format("Sun shadows {:.2f}: sun rays (and debug views' primary rays) {:.2f}, soft shadow filter {:.2f}, adding {:.2f}",
        c.sunShadows(), c.sunRays, c.sunFilter, c.sunInject));
    lines.push_back(std::format("Ambient occlusion {:.2f}: rays {:.2f}, packing and blur {:.2f}", c.ambientOcclusion(), c.ambientRays,
        c.ambientFilter));
    lines.push_back(std::format("Global illumination {:.2f}: rays {:.2f}, accumulation and filter {:.2f}, adding {:.2f}", c.globalIllumination(),
        c.bounceRays, c.bounceFilter, c.bounceAdd));
    lines.push_back(std::format("Light shadows {:.2f}: rays {:.2f}, Direct3D 12 hand-over {:.2f}, soft shadow and bounce filters {:.2f}, "
        "constants and adding {:.2f}; {} lights traced, {} left to the game", c.lightShadows(), c.lightRays, c.lightSync, c.lightFilter, c.lightAdd,
        state.tracedLights, state.gameLights));
    lines.push_back(std::format("Debug view {:.2f}", c.debugView));
    lines.push_back(std::format("CPU {:.2f}: repeating draws {:.2f}, preparing and submitting the trace {:.2f} (foliage {:.2f}, cache plans and "
        "instances {:.2f}, Direct3D 11 flush {:.2f}, record and submit {:.2f}, preparing {:.2f}, other {:.2f}; waiting for the GPU {:.2f})",
        c.cpu(), c.captureCpu, c.traceCpu, c.foliageCpu, c.instancesCpu, c.flushCpu, c.submitCpu, c.prepareCpu, c.otherTraceCpu(), c.gpuWaitCpu));
    lines.push_back(std::format("Work: {:.2f} M triangles rebuilt (view {:.2f} M, shadow casters {:.2f} M), {} draws captured, {} cache entries built, "
        "{} cached draws traced, {} foliage instances", static_cast<double>(state.triangles) / 1e6, static_cast<double>(state.viewTriangles) / 1e6,
        static_cast<double>(state.casterTriangles) / 1e6, state.capturedDraws + state.casterDraws, state.newCacheEntries, state.tracedCachedDraws,
        state.foliageInstances));

    const auto& m = state.measurement;
    if (m.running) lines.push_back(std::format("Real cost: measuring, step {} of {}", m.step + 1, m.stepCount));
    else if (m.steps.size() < 2) lines.emplace_back("Real cost: not measured");
    else
    {
        auto line = std::format("Real cost: everything on {:.2f}; leaving out", m.steps.front().frame);
        for (std::size_t i = 1; i < m.steps.size(); ++i)
            line += std::format("{} {} saves {:.2f}", i > 1 ? "," : "", m.steps[i].removed, m.steps[i - 1].frame - m.steps[i].frame);
        lines.push_back(line + std::format("; total {:.2f}", m.steps.front().frame - m.steps.back().frame));
    }
    return lines;
}

void logFrame(const Capture& frame)
{
    const auto state = status();
    char text[1536]{};
    std::snprintf(text, sizeof(text), "Ray tracing frame %llu: %u G-buffer draws, %u captured (%u alpha-tested, %u of them tiled, %u without coverage, %u materials), %u decals, sky or screen passes, "
        "%u unknown shader, %u unsupported, %u over capacity; %u shadow casters (%u repeats skipped, %u left to the view, %u to the cache), %u cached draws (%u traced, %u standing in for the view; not cached: %u moving or repeated, %u anonymous), %u foliage instances from %u meshes (%u placed; %u draws left to them; %llu tree sections past %u not traced so far); %llu triangles; "
        "%u shadow resolve draws, sun %s %.3f, %.3f, %.3f; traced shadows %s; traced ambient occlusion %s; traced global illumination %s; "
        "local lights %u traced, %u left to the game.",
        static_cast<unsigned long long>(tracedFrames), frame.basePassDraws, frame.capturedDraws, frame.maskedDraws,
        static_cast<unsigned>(frame.transformDraws.size()), frame.unbakedDraws,
        state.bakedMaterials, frame.backgroundDraws,
        frame.unknownShaderDraws, frame.unsupportedDraws, frame.overCapacityDraws, frame.casterDraws, frame.repeatedCasterDraws, frame.casterDrawsInView,
        frame.casterDrawsInCache, state.cachedDraws, state.tracedCachedDraws, frame.standInDraws, frame.movingDraws, frame.anonymousDraws, state.foliageInstances,
        gpu ? static_cast<unsigned>(gpu->foliageTemplates.size()) : 0u,
        gpu ? static_cast<unsigned>(std::count_if(gpu->foliageTemplates.begin(), gpu->foliageTemplates.end(),
            [](const auto& model) { return model.second.reference != NoInstance && gpu->templates->find(model.second.cacheKey); })) : 0u,
        frame.copiedFoliageDraws, gpu ? static_cast<unsigned long long>(gpu->foliageSectionsIgnored) : 0ull, MaxTreeSections,
        static_cast<unsigned long long>((frame.opaqueVertices + frame.maskedVertices) / 3), frame.shadowResolveDraws,
        state.sunFound ? "at" : "not found,", state.sun[0], state.sun[1], state.sun[2], frame.shadowsInjected ? "added" : "off",
        frame.ambientInjected ? "in use" : "off", frame.bounceAdded ? "added" : "off", frame.tracedLights, frame.gameLights);
    upscaler::log(text);
    const auto lines = costLines(state);
    std::string summary = "Ray tracing costs:";
    for (std::size_t i = 1; i < lines.size(); ++i) summary += (i > 1 ? " | " : " ") + lines[i];
    upscaler::log(summary);
}

void releaseGpu()
{
    if (!gpu) return;

    try { waitIdle(*gpu); }
    catch (const std::exception& error) { upscaler::log(error.what()); }
    gpu.reset();
    upscaler::log("Ray tracing released its resources.");
}
}

bool wanted() noexcept
{
    const auto features = activeFeatures();
    return (selected != View::Off || features.sunShadows || features.ambientOcclusion || features.globalIllumination || features.lightShadows
        || (measuring && !bypassed)) && !failed;
}

void beginView(const float (&clipFromWorld)[16], const std::int32_t (&rect)[4], const std::int32_t (&unscaled)[4],
    const float (&preViewTranslation)[3], bool cameraKnown) noexcept
{
    if (!wanted()) return;

    capture.reset();
    capture.open = true;
    std::memcpy(capture.clipFromWorld, clipFromWorld, sizeof(capture.clipFromWorld));
    std::memcpy(capture.preViewTranslation, preViewTranslation, sizeof(capture.preViewTranslation));
    capture.cameraKnown = cameraKnown;
    std::memcpy(capture.rect, rect, sizeof(capture.rect));
    std::memcpy(capture.unscaled, unscaled, sizeof(capture.unscaled));
}

// The constant buffer slots a shader's program declares (dcl_constantbuffer), as a mask.
UINT declaredConstantBuffers(const void* code, std::size_t size)
{
    constexpr std::uint32_t ConstantBufferDeclaration = 89;
    const auto* bytes = static_cast<const std::uint8_t*>(code);
    std::uint32_t chunks{};
    if (size < 32 || std::memcmp(bytes, "DXBC", 4)) return 0;

    std::memcpy(&chunks, bytes + 28, 4);
    for (std::uint32_t c = 0; c < chunks && 32 + (c + 1) * 4 <= size; ++c)
    {
        std::uint32_t offset{}, length{};
        std::memcpy(&offset, bytes + 32 + c * 4, 4);
        if (std::uint64_t{offset} + 8 > size) return 0;

        std::memcpy(&length, bytes + offset + 4, 4);
        if ((std::memcmp(bytes + offset, "SHEX", 4) && std::memcmp(bytes + offset, "SHDR", 4)) || offset + 8 + std::uint64_t{length} > size)
            continue;

        std::vector<std::uint32_t> words(length / 4);
        std::memcpy(words.data(), bytes + offset + 8, words.size() * 4);
        UINT mask{};
        // The opcode token, the operand token, then the slot.
        for (std::size_t i = 2, step = 0; i + 2 < words.size(); i += step)
        {
            step = upscaler::instructionLength(std::span(words).subspan(i));
            if (!step) break;
            if ((words[i] & 0x7ff) == ConstantBufferDeclaration && words[i + 2] < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
                mask |= 1u << words[i + 2];
        }
        return mask;
    }
    return 0;
}

void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3DBlob> signature;
    if (!code || !shader || FAILED(shader->QueryInterface(IID_PPV_ARGS(&vertexShader)))
        || FAILED(D3DGetOutputSignatureBlob(code, size, signature.GetAddressOf())))
        return;

    vertexShader->SetPrivateData(VertexBytecode, static_cast<UINT>(size), code);
    if (size >= 20)
    {
        const auto* checksum = static_cast<const std::uint8_t*>(code) + 4;
        for (const auto& [hash, kind] : {std::pair{LightVolumeShader, LightShader::Volume}, std::pair{DeferredLightShader, LightShader::Light}})
            if (std::equal(hash.begin(), hash.end(), checksum)) vertexShader->SetPrivateData(LightShaderTag, sizeof(kind), &kind);
    }
    const auto slots = declaredConstantBuffers(code, size);
    vertexShader->SetPrivateData(ConstantSlots, sizeof(slots), &slots);

    try
    {
        const std::string key(static_cast<const char*>(signature->GetBufferPointer()), signature->GetBufferSize());
        ComPtr<ID3D11GeometryShader> streamOutput;
        {
            std::lock_guard lock(streamOutputMutex);
            auto& cached = streamOutputShaders[key];
            if (!cached)
            {
                // Without a geometry shader, stream output takes the vertex shader's outputs as they are.
                ComPtr<ID3D11Device> device;
                vertexShader->GetDevice(device.GetAddressOf());
                const D3D11_SO_DECLARATION_ENTRY position{0, "SV_Position", 0, 0, 4, 0};
                const UINT stride = VertexBytes;
                device->CreateGeometryShaderWithStreamOutput(code, size, &position, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
                    cached.GetAddressOf());
            }
            streamOutput = cached;
        }
        if (streamOutput) vertexShader->SetPrivateDataInterface(StreamOutputShader, streamOutput.Get());
    }
    catch (...) {}
}

void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    ComPtr<ID3D11PixelShader> pixelShader;
    if (!code || size < 20 || !shader || std::memcmp(code, "DXBC", 4) || FAILED(shader->QueryInterface(IID_PPV_ARGS(&pixelShader))))
        return;

    const auto* checksum = static_cast<const std::uint8_t*>(code) + 4;
    for (const auto& resolve : ShadowResolves)
        if (std::equal(resolve.checksum.begin(), resolve.checksum.end(), checksum))
            pixelShader->SetPrivateData(ShadowResolveShader, sizeof(resolve.constantSlot), &resolve.constantSlot);
    try
    {
        // Light shaders for hair keep the light's constants in another slot.
        const auto slots = declaredConstantBuffers(code, size);
        pixelShader->SetPrivateData(ConstantSlots, sizeof(slots), &slots);
        constexpr UINT tested = 1;
        // Alpha-tested materials discard by a texture; their cards are not solid.
        if (!discardsByTexture(code, size)) return;

        pixelShader->SetPrivateData(AlphaTestedShader, sizeof(tested), &tested);
        const auto test = analyseAlphaTest(code, size);
        if (!test || test->coordinates.semantic.size() >= sizeof(BakeInput::semantic)
            || (test->fade && test->fade->semantic.size() >= sizeof(BakeInput::fadeSemantic)))
            return;

        BakeInput input{};
        std::memcpy(input.semantic, test->coordinates.semantic.c_str(), test->coordinates.semantic.size());
        input.semanticIndex = test->coordinates.semanticIndex;
        input.component = test->firstComponent;
        input.textures = test->textures;
        if (test->fade)
        {
            std::memcpy(input.fadeSemantic, test->fade->semantic.c_str(), test->fade->semantic.size());
            input.fadeSemanticIndex = test->fade->semanticIndex;
            input.fadeComponent = test->fadeComponent;
        }
        input.toggles = static_cast<UINT>(test->toggles.size());
        input.transformed = test->transform.has_value();
        if (test->transform) input.transform = *test->transform;
        pixelShader->SetPrivateData(BakeInputData, sizeof(input), &input);
        pixelShader->SetPrivateData(BakeShaderSource, static_cast<UINT>(test->bakeVertexShader.size()), test->bakeVertexShader.data());
    }
    catch (...) {}
}

void computeShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    ComPtr<ID3D11ComputeShader> computeShader;
    if (!code || size < 20 || !shader || std::memcmp(code, "DXBC", 4) || FAILED(shader->QueryInterface(IID_PPV_ARGS(&computeShader))))
        return;

    const auto* checksum = static_cast<const std::uint8_t*>(code) + 4;
    for (const auto& ambient : AmbientShaders)
        if (std::equal(ambient.checksum.begin(), ambient.checksum.end(), checksum))
            computeShader->SetPrivateData(AmbientShader, sizeof(ambient.pass), &ambient.pass);
}

void dispatch(ID3D11DeviceContext* context) noexcept
{
    if (!capture.open || !gpu || context != gpu->context) return;

    try
    {
        if (capture.lightsPending) traceLights(*gpu, capture);
        // Compute work after lighting has started means every lighting model has drawn.
        if (capture.bouncePending) addBounce(*gpu, capture);
        if (capture.traced) return;

        ComPtr<ID3D11ComputeShader> shader;
        AmbientPass pass{};
        UINT size = sizeof(pass);
        context->CSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        if (!shader || FAILED(shader->GetPrivateData(AmbientShader, &size, &pass))) return;

        if (pass == AmbientPass::Mask)
        {
            ComPtr<ID3D11ShaderResourceView> normals;
            context->CSGetShaderResources(0, 1, normals.GetAddressOf());
            capture.normals.Reset();
            if (normals) normals->GetResource(capture.normals.GetAddressOf());
            return;
        }
        ComPtr<ID3D11UnorderedAccessView> target;
        context->CSGetUnorderedAccessViews(0, 1, target.GetAddressOf());
        capture.ambient.Reset();
        if (target) target->GetResource(capture.ambient.GetAddressOf());
        gpu->ambient.blur = shader;
    }
    catch (const std::exception& error) { fail(error.what()); }
}

// Before the game's draw: traces once lighting reads the sun shadow, and captures the geometry before.
void captureDraw(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances, std::uint32_t first, std::int32_t baseVertex,
    std::uint32_t firstInstance, bool indexed)
{
    if (capture.shadowPending && readsShadow(context)) shadowsResolved(*gpu);
    // Lighting draws a full-screen pass per lighting model, each reading the sun shadow.
    else if (capture.bouncePending && !readsShadow(context)) addBounce(*gpu, capture);
    if (capture.ambient && !capture.ambientRead && ambientWanted() && readsResource(context, capture.ambient.Get())) ambientRead(*gpu);
    // Geometry drawn after the trace would not be in this frame's structure.
    if (capture.traced) return;

    // Bend's G-buffer is two packed targets (R32G32_UINT, R32_UINT), with R16G16_UNORM velocity
    // added for moving objects. Depth prepass, shadow and velocity passes draw to fewer.
    ID3D11RenderTargetView* targets[3]{};
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(3, targets, depth.GetAddressOf());
    const bool basePass = targets[1] && depth, depthOnly = !targets[0] && depth, moving = targets[2];
    if (basePass && targets[0] && !capture.gbuffer[0])
        for (int i = 0; i < 2; ++i) targets[i]->GetResource(capture.gbuffer[i].GetAddressOf());
    for (auto* target : targets)
        if (target) target->Release();
    LARGE_INTEGER start{}, end{};
    QueryPerformanceCounter(&start);
    const auto finish = [&]
    {
        QueryPerformanceCounter(&end);
        capture.captureTicks += end.QuadPart - start.QuadPart;
    };
    D3D11_VIEWPORT viewport{};
    const auto map = depthOnly ? shadowMapDraw(context, depth.Get(), viewport) : std::nullopt;
    if (map)
    {
        captureShadowCaster(context, *map, viewport, count, instances, first, baseVertex, firstInstance, indexed);
        return finish();
    }
    // Bend alpha-tests in its depth-only passes; the G-buffer pass then matches their depth
    // without discarding. The prepass draws carry the coordinates and material the coverage needs.
    if (depthOnly && alphaTested(context))
    {
        const auto key = drawKey(context, count, instances, first, baseVertex, firstInstance, indexed);
        capture.maskedMeshes.insert(key);
        ComPtr<ID3D11Resource> target;
        depth->GetResource(target.GetAddressOf());
        const auto firstVertex = capture.maskedVertices;
        if (target.Get() != gpu->sceneDepth.Get() || !capture.casterRuns.empty()) return finish();

        // Foliage is told apart only for the trees' copies, which only the off-screen geometry traces.
        if (const auto foliage = offscreenGeometry ? foliageSource(context) : std::nullopt)
        {
            const auto mesh = foliageMesh(context, count, first, baseVertex, true);
            if (copiesStandIn(*gpu, mesh))
            {
                ++capture.copiedFoliageDraws;
                recordFoliage(context, *foliage, mesh, instances, firstInstance, 0, 0, NoMaterial, true);
                return finish();
            }
        }
        const auto object = objectKey(context, instances, firstInstance);
        const bool repeated = drawnBefore(object, key);
        if (!repeated && cacheStandsIn(*gpu, object, key))
        {
            standIn(object, key, true);
            return finish();
        }
        if (captureAlphaTested(context, count, instances, first, baseVertex, firstInstance, indexed))
        {
            ++capture.maskedDraws;
            ++capture.capturedDraws;
            capture.capturedKeys.insert(key);
            const auto vertices = capture.maskedVertices - firstVertex;
            const auto slot = capture.materialDraws.back()[1];
            offerToCache(object, key, firstVertex, vertices, slot, true, repeated);
            if (const auto foliage = offscreenGeometry ? foliageSource(context) : std::nullopt)
                recordFoliage(context, *foliage, foliageMesh(context, count, first, baseVertex, true), instances, firstInstance, firstVertex,
                    vertices, slot, true);
        }
        return finish();
    }
    if (!basePass) return;

    ++capture.basePassDraws;
    if (!capture.depth) depth->GetResource(capture.depth.GetAddressOf());
    if (!opaqueGeometry(context))
    {
        ++capture.backgroundDraws;
        return finish();
    }
    // Alpha-tested meshes are captured from the depth prepass, with their coordinates.
    if (alphaTested(context) || capture.maskedMeshes.contains(drawKey(context, count, instances, first, baseVertex, firstInstance, indexed)))
        return finish();
    // The view's geometry precedes the shadow casters in both ranges.
    if (!capture.casterRuns.empty())
    {
        ++capture.unsupportedDraws;
        return finish();
    }

    // Foliage sways with the wind, drawn with velocity, but its instances stay put.
    if (const auto foliage = offscreenGeometry ? foliageSource(context) : std::nullopt)
    {
        const auto mesh = foliageMesh(context, count, first, baseVertex, false);
        if (copiesStandIn(*gpu, mesh))
        {
            ++capture.copiedFoliageDraws;
            recordFoliage(context, *foliage, mesh, instances, firstInstance, 0, 0, NoMaterial, false);
            return finish();
        }
    }
    const auto key = drawKey(context, count, instances, first, baseVertex, firstInstance, indexed);
    const auto object = objectKey(context, instances, firstInstance);
    const bool repeated = drawnBefore(object, key);
    if (!moving && !repeated && cacheStandsIn(*gpu, object, key))
    {
        standIn(object, key, false);
        return finish();
    }
    const auto firstVertex = capture.opaqueVertices;
    if (captureOpaque(context, count, instances, first, baseVertex, firstInstance, indexed))
    {
        ++capture.capturedDraws;
        capture.capturedKeys.insert(key);
        const auto vertices = capture.opaqueVertices - firstVertex;
        offerToCache(object, key, firstVertex, vertices, NoMaterial, false, moving || repeated);
        // Foliage sways with the wind, drawn with velocity, but its instances stay put.
        if (const auto foliage = offscreenGeometry ? foliageSource(context) : std::nullopt)
            recordFoliage(context, *foliage, foliageMesh(context, count, first, baseVertex, false), instances, firstInstance, firstVertex,
                vertices, NoMaterial, false);
    }
    finish();
}

bool draw(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances, std::uint32_t first, std::int32_t baseVertex,
    std::uint32_t firstInstance, bool indexed) noexcept
{
    if (!capture.open || !gpu || context != gpu->context) return false;

    try
    {
        if (!std::exchange(capture.foliagePlaced, true)) prepareFoliage(*gpu, capture);
        if (shadowResolve(context))
        {
            const auto features = activeFeatures();
            return features.sunShadows && features.replaceCanopyShadows && gpu->sunFound
                && resolveWithoutCanopy(context, count, instances, first, baseVertex, firstInstance, indexed);
        }
        // The local lights draw after the trace; any other draw after them means they all have.
        const auto light = capture.traced ? lightShader(context) : std::nullopt;
        if (capture.lightsPending && !light) traceLights(*gpu, capture);
        captureDraw(context, count, instances, first, baseVertex, firstInstance, indexed);
        if (!light) capture.lightOpen = false;
        else if (*light == LightShader::Volume) capture.lightVolumeDrawn = true;
        else if (activeFeatures().lightShadows && capture.cameraKnown)
            return drawLight(*gpu, capture, context, count, instances, first, baseVertex, firstInstance, indexed);
    }
    catch (const std::exception& error) { fail(error.what()); }
    return false;
}

void present(ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer) noexcept
{
    const auto view = selected.load();
    auto& frame = capture;
    const auto now = ticksNow();
    const auto frameMilliseconds = lastPresent ? millisecondsSince(now - lastPresent) : 0.0f;
    lastPresent = now;
    bool traced{};
    try
    {
        // The A/B key only pauses the features, so switching back is instant.
        if (failed || (view == View::Off && !sunShadows && !ambientOcclusion && !globalIllumination && !lightShadows && !measuring)) releaseGpu();
        else
        {
            if (!gpu) gpu = create(context);
            if (frame.depth) gpu->sceneDepth = frame.depth;
            readSun(*gpu);
            if (frame.shadowPending) copyShadow(*gpu, frame);
            // Lights still in their slots at the end of the frame are lost: the scene colour has been
            // used by now. Nothing after the lights drew to it, or ran compute work.
            if (std::exchange(frame.lightsPending, false))
            {
                static bool reported{};
                if (!std::exchange(reported, true)) upscaler::log("The frame ended before the traced lights could be added to the scene.");
            }
            // Frames without a sun shadow pass (menus, interiors) are traced here for the debug views.
            traced = frame.traced || (view != View::Off && trace(*gpu, frame));
            if (traced && view != View::Off && backBuffer)
            {
                gpu->timer->begin(gpu->context, Pass::DebugView);
                composite(*gpu, frame, backBuffer, view);
                gpu->timer->end(gpu->context, Pass::DebugView);
            }
            if (traced) ++tracedFrames;
            publish(frame);
            if (traced && (tracedFrames == 1 || tracedFrames % 600 == 0)) logFrame(frame);
        }
        sampleCosts(frame, traced, frameMilliseconds);
        measure(now);
        publishCosts();
    }
    catch (const std::exception& error) { fail(error.what()); }

    // The draws until the next view begins are not captured.
    capture.reset();
}

void release() noexcept
{
    capture = {};
    releaseGpu();
    std::lock_guard lock(streamOutputMutex);
    streamOutputShaders.clear();
}

Status status()
{
    std::lock_guard lock(statusMutex);
    auto result = published;
    result.view = selected;
    result.features.sunShadows = sunShadows;
    result.features.ambientOcclusion = ambientOcclusion;
    result.features.halfAmbientRays = halfAmbientRays;
    result.features.replaceCanopyShadows = replaceCanopyShadows;
    result.features.offscreenGeometry = offscreenGeometry;
    result.features.globalIllumination = globalIllumination;
    result.features.lightShadows = lightShadows;
    result.features.softLightShadows = softLightShadows;
    result.features.lightBounce = lightBounce;
    result.features.softSunShadows = softSunShadows;
    result.bypassed = bypassed;
    result.toggledAt = toggledAt;
    result.performanceOverlay = performanceOverlay;
    return result;
}

void select(View view)
{
    {
        std::lock_guard lock(statusMutex);
        published.failure.clear();
    }
    failed = false;
    selected = view;
}

void enable(const Features& features)
{
    {
        std::lock_guard lock(statusMutex);
        published.failure.clear();
    }
    failed = false;
    sunShadows = features.sunShadows;
    ambientOcclusion = features.ambientOcclusion;
    halfAmbientRays = features.halfAmbientRays;
    replaceCanopyShadows = features.replaceCanopyShadows;
    offscreenGeometry = features.offscreenGeometry;
    globalIllumination = features.globalIllumination;
    lightShadows = features.lightShadows;
    softLightShadows = features.softLightShadows;
    lightBounce = features.lightBounce;
    softSunShadows = features.softSunShadows;
}

void toggleFeatures()
{
    bypassed = !bypassed;
    toggledAt = GetTickCount64();
    upscaler::log(std::string("A/B: ray-traced features ") + (bypassed ? "off." : "on."));
}

void showPerformance(bool show)
{
    performanceOverlay = show;
}

void measureCosts()
{
    measurementRequested = true;
}

std::string performanceReport()
{
    std::string report;
    for (const auto& line : costLines(status())) report += line + '\n';
    return report;
}
}
