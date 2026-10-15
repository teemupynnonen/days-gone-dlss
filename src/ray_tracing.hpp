#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IUnknown;

// Path-tracing experiment. Each frame the G-buffer and sun shadow map draws are repeated with stream
// output, a D3D12 device sharing the GPU builds DXR acceleration structures from the captured
// triangles and traces one ray per pixel plus one toward the sun, four short ones (or two) for ambient
// occlusion and one bounce of sunlight for global illumination. Features feed the result into the
// game's lighting; debug views draw it over the frame. docs/DEVELOPMENT.md describes the route.
namespace days_gone_dlss::ray_tracing
{
enum class View : std::uint8_t
{
    Off, Geometry, DepthComparison, SunShadows, ShadowComparison, ShadowCasters, AmbientOcclusion, AmbientComparison, OffscreenShadows,
    GlobalIllumination
};

// Changes to the game's own image, each switched on separately and all off together with the A/B key.
struct Features
{
    // Sun shadows traced from the geometry the game draws for the view and for its sun shadow map,
    // added to the game's own.
    bool sunShadows{};
    // Ambient occlusion traced from the same geometry in place of the game's screen-space occlusion,
    // filtered by the game's own blur and temporal accumulation.
    bool ambientOcclusion{};
    // With ambient occlusion: each pixel traces two of its four directions and takes the other two
    // from its neighbour, for about half the rays and a little more noise.
    bool halfAmbientRays{};
    // With sun shadows: leaves out the game's soft tree shadow, which comes from a light-space map of
    // the canopies, so the traced trees shadow alone.
    bool replaceCanopyShadows{};
    // Traces the cached static draws and the copies of the trees around the camera too, so their
    // shadows stay when they leave the view. Without it, only this frame's view and the game's shadow
    // casters cast.
    bool offscreenGeometry{true};
    // Sunlight bounced once off the same geometry, added to the game's lighting.
    bool globalIllumination{};

    bool operator==(const Features&) const = default;
};

// Where the time goes, in milliseconds, averaged over about the last second. GPU times come from
// timestamps on both devices, which take turns on the GPU, so they add up. Not timed: the repeated
// draws' GPU work, which runs inside the game's own passes. The cost measurement includes it.
struct Costs
{
    float frame{}; // Between presents: the game's whole frame
    // On the thread running Direct3D work: repeating the game's draws, and preparing and submitting
    // the trace, including waiting for the GPU to finish an earlier frame's.
    float captureCpu{}, traceCpu{}, gpuWaitCpu{};
    // The scene every feature traces: placing the captured vertices and copying the depth, copying
    // them to Direct3D 12, building the view's and the shadow casters' structures, the geometry
    // cache's and the foliage templates' new entries and the top-level structure, and Direct3D 11
    // waiting on Direct3D 12 beyond its work.
    float prepare{}, copy{}, viewBuild{}, casterBuild{}, cacheBuild{}, templateBuild{}, topBuild{}, sync{};
    float sunRays{}, sunInject{}; // The primary and sun rays; adding them to the game's shadow
    float ambientRays{}, ambientFilter{}; // Packing in the game's format and its blur
    float bounceRays{}, bounceFilter{}, bounceAdd{}; // Accumulation and filter; adding it to the scene
    float debugView{};

    float scene() const { return prepare + copy + viewBuild + casterBuild + cacheBuild + templateBuild + topBuild + sync; }
    float sunShadows() const { return sunRays + sunInject; }
    float ambientOcclusion() const { return ambientRays + ambientFilter; }
    float globalIllumination() const { return bounceRays + bounceFilter + bounceAdd; }
    float gpu() const { return scene() + sunShadows() + ambientOcclusion() + globalIllumination() + debugView; }
    float cpu() const { return captureCpu + traceCpu; }
};

// The real cost of each feature: the frame time with everything on, then with the features switched
// off one by one, the scene alone (captured and built, nothing traced), and ray tracing paused. Each
// step settles, then is timed. The difference between steps is what the feature left out costs,
// everything included. Steps whose feature is already off are skipped.
struct CostMeasurement
{
    struct Step { std::string removed; float frame{}; }; // The first step removes nothing
    std::vector<Step> steps;
    std::uint64_t finishedAt{}; // GetTickCount64 when `steps` were measured
    bool running{};
    unsigned step{}, stepCount{};
};

struct Status
{
    View view{};
    Features features;
    bool bypassed{}; // The A/B key has switched the features off
    std::uint64_t toggledAt{}; // GetTickCount64 of the last A/B switch
    bool performanceOverlay{};
    std::string failure;
    std::uint64_t tracedFrames{};
    // The last traced frame. Base-pass draws are the G-buffer draws; each one is captured or skipped.
    unsigned basePassDraws{}, capturedDraws{}, maskedDraws{}, unbakedDraws{}, bakedMaterials{}, backgroundDraws{}, unknownShaderDraws{},
        unsupportedDraws{}, overCapacityDraws{};
    unsigned casterDraws{}; // Captured from the sun shadow maps
    unsigned casterDrawsInView{}, casterDrawsInCache{}; // Left to the view's capture of their object, and to the geometry cache
    unsigned cachedDraws{}, tracedCachedDraws{}; // Static draws kept from earlier frames, and those traced off view
    unsigned newCacheEntries{}; // Built this frame, in the geometry cache and as foliage templates
    unsigned foliageInstances{}; // Tree sections placed from their meshes' templates
    std::uint64_t triangles{}, capacity{};
    std::uint64_t viewTriangles{}, casterTriangles{}; // Rebuilt every frame
    unsigned shadowResolveDraws{};
    bool sunFound{}, shadowsInjected{};
    float sun[3]{}; // Toward the sun or moon that casts the game's directional shadows
    bool ambientInjected{}; // The traced ambient occlusion replaced the game's this frame
    bool bounceAdded{}; // The traced global illumination lit the scene this frame
    Costs costs;
    CostMeasurement measurement;
};

// ---- Engine side ----
// True while a view or feature is on; the engine hook then reports each frame's view.
bool wanted() noexcept;
// At the RHI command preceding the frame's scene rendering. `clipFromWorld` is the jittered
// FViewMatrices::TranslatedViewProjectionMatrix (row vectors); rects are in buffer pixels.
// Translated world is world plus `preViewTranslation`, the negated camera position, when known.
void beginView(const float (&clipFromWorld)[16], const std::int32_t (&rect)[4], const std::int32_t (&unscaled)[4],
    const float (&preViewTranslation)[3], bool cameraKnown) noexcept;

// ---- Render side, on the thread executing Direct3D work ----
// Prepares each vertex shader for capture when it is created, on any thread.
void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Recognises the game's sun shadow resolve and alpha-tested materials, on any thread.
void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Recognises the game's screen-space ambient occlusion passes, on any thread.
void computeShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Notes the G-buffer normal and the ambient occlusion target of the game's occlusion passes, and adds
// the bounced light when compute work follows lighting.
void dispatch(ID3D11DeviceContext* context) noexcept;
// Repeats a G-buffer draw with stream output before the game's own draw runs. After the sun shadow
// resolve it traces the frame and, with the features on, adds the traced shadows and ambient
// occlusion before lighting and the bounced light after it. Replacing the canopy shadows, it draws the sun shadow resolve itself:
// returns true then, and the game's draw is skipped.
bool draw(ID3D11DeviceContext* context, std::uint32_t count, std::uint32_t instances, std::uint32_t first, std::int32_t baseVertex,
    std::uint32_t firstInstance, bool indexed) noexcept;
// Draws the selected debug view over the back buffer, tracing first if the frame had no sun shadow pass.
void present(ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer) noexcept;
// Releases every resource; only when no Direct3D work can run (device teardown).
void release() noexcept;

// ---- Settings UI ----
Status status();
void select(View view);
void enable(const Features& features);
// The A/B key: switches every enabled feature off, or back on.
void toggleFeatures();
// Shows the costs over the game, even with the overlay closed.
void showPerformance(bool show);
// Starts measuring each feature's real cost, unless a measurement is running.
void measureCosts();
// The features, as a section of the DLSS overlay window.
void drawMenu();
// The debug view, costs and statistics, inside the window's Diagnostics section.
void drawDiagnostics(float width);
// Drawn even with the overlay closed: a short note of the A/B state after the key is pressed, and
// the costs when shown.
void drawIndicator();
// The costs as plain text, for DaysGoneDLSS-performance.txt.
std::string performanceReport();
}
