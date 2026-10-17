# Days Gone DLSS development

## Build

Requires Visual Studio 2022 (or Build Tools) with the C++ workload and CMake 3.25 or later.
From a x64 developer prompt:

```
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release
```

CMake downloads MinHook, the DLSS SDK v310.9.1 headers, loader and `nvngx_dlss.dll`, the
ReShade 6.8.0 add-on headers, and Microsoft's DXBC checksum implementation, each pinned
by commit and checked by SHA-256. Frame generation needs the Streamline SDK 2.14.1, which only
its release archive carries: the first configure downloads it once (about 280 MB, checked by
SHA-256) and keeps the headers, license and x64 runtime files. FSR needs the AMD FSR SDK 2.3.0's API
headers and AMD's signed Direct3D 12 DLLs, pinned by commit and checked by SHA-256 like the rest
(about 66 MB). The ray-tracing experiment's trace
shader is compiled with the Windows SDK's `dxc`, which the developer prompt puts on the PATH. The
add-on is written to `build/release/DaysGoneDLSS.addon64` together with `nvngx_dlss.dll`,
Streamline's `sl.*.dll` and `nvngx_dlssg.dll`, and AMD's `amd_fidelityfx_*_dx12.dll`.

`cmake --install build/release --component addon --prefix <dir>` collects the add-on, its DLLs,
player documentation and licenses for distribution. Runtime files are at
the install root; documentation and licenses are in `DaysGoneDLSS/`.

## Package a release

From the repository root, after building and testing:

```
cpack --config build/release/CPackConfig.cmake -B build/release
```

This produces `build/release/DaysGoneDLSS-1.6.0.zip` and its `.sha256` checksum. CPack
packages only the `addon` component, so SDK headers, static libraries, tests, debug symbols,
and local settings are excluded. ReShade is installed separately by the player.

## Sources

| File | Purpose |
| --- | --- |
| `src/upscaler.cpp`, `src/upscaler.hpp` | Core: engine hooks, shader identification, DLSS evaluation and composite |
| `src/upscaler_shaders.hpp` | HLSL for the input preparation and the SDR/HDR composites |
| `src/upscaler_addon.cpp` | ReShade add-on: events, exports, settings location |
| `src/upscaler_menu.cpp` | Settings window in the ReShade overlay |
| `src/menu_style.hpp` | Colours, tooltips and status lines the overlay's sections share |
| `src/upscaler_settings.cpp`, `src/upscaler_settings.hpp` | Modes, render scales and `DaysGoneDLSS.ini` |
| `src/presenter.cpp`, `src/presenter.hpp` | Frame generation: the game's stand-in swap chain, presented through Direct3D 12 |
| `src/frame_generation.cpp`, `src/frame_generation.hpp` | DLSS Frame Generation: Streamline, its inputs and camera, Reflex |
| `src/fidelityfx.cpp`, `src/fidelityfx.hpp` | Loads the AMD FSR API |
| `src/fsr_upscaler.cpp`, `src/fsr_upscaler.hpp` | FSR upscaling on Direct3D 12 for the upscaler |
| `src/fsr_frame_generation.cpp`, `src/fsr_frame_generation.hpp` | FSR frame generation: AMD's swap chain and its inputs |
| `tests/upscaler_tests.cpp` | Settings tests |
| `src/shader_bytecode.*` | Bounded DXBC parsing and checksum updates |
| `src/ambient_occlusion.*`, `tests/ambient_occlusion_tests.cpp` | AO reconstruction weight correction and WARP regression tests |
| `src/indirect_lighting.*` | Identifies 1.07's screen-space indirect lighting passes |
| `src/ray_tracing*`, `src/alpha_test.*`, `tests/ray_tracing_tests.cpp` | Ray-tracing experiment: geometry capture, alpha-test baking, DXR trace, sun shadows, global illumination and debug views |
| `src/pass_timer.*` | GPU timing of the ray tracer's Direct3D 11 passes |

The shader rendering test uses authored HLSL on WARP to check the AO near-integer
rounding regression. The ray-tracing test needs a DXR 1.1 GPU and is skipped without one: it
captures authored draws, including an alpha-tested quad with a per-vertex fade drawn with a
narrowed depth range, and requires the traced pixels to match the rasterised coverage and depth. No extracted game shaders are required by the tests.

## Game versions

The add-on supports 1.13 (Steam build 19221447) and 1.07 (Steam beta `days_gone_classic`, build
10034136), identified by the executable's PE header when the add-on loads. `Builds[]` in
`src/upscaler.cpp` holds each build's addresses, FViewInfo layout and shader checksums, and the
runtime HLSL is compiled with `GAME_VERSION` 113 or 107. The rest of this document describes 1.13;
[DIFFERENCES.md](DIFFERENCES.md) lists what 1.07 does differently.

## How Bend renders anti-aliasing

Days Gone runs Unreal 4.11 with Bend's own post-processing on Direct3D 11:

1. `FDepthResolveCS` combines device depth with the GBuffer velocity into a packed `R32_UINT`
   motion texture for the TAA: 14-bit X, 13-bit Y of the history UV offset (about 0.06 pixels
   at 4K), dilated to the nearest depth in 3×3, plus flags. Zero means no usable history:
   off-screen, or faster than a dithered 6.7 to 11 percent of the screen per frame. The top two
   bits count matching previous-frame depths and are zero where history is rejected, including
   pixels marked as moving (bit 31 of its `t0` input) that drew no object velocity. Camera
   motion comes from depth and the view's previous matrices, so it is complete.
2. Tonemapping writes a gamma-2 (square-root) encoded colour target, `R16G16B16A16_FLOAT`.
3. The TAA job runs after Slate has drawn the UI into its own target. At full resolution
   `TBendSMAATemporalAACS_T1X_0000` resolves TAA in YCoCg, then squares the colour and applies
   vignette, film grain, the Slate UI and sRGB in the same dispatch. Below full resolution
   `_0001` only updates a render-resolution history, and `TBendSMAABufferCopyPS_01`
   (sharpening) or `_00` (95-110 percent) upscales it and does the same composite.
4. With HDR output on, full resolution uses `_0010` and the composite `_10`/`_11`; `_0001` is
   shared. The HDR variants keep scene and UI brightness in constant register 5 (the composite
   PS also its blend flag in `.z`), shift the later registers by one and write PQ: a soft toe
   into ST 2084 with 1.0 at 200 nits.

Jitter comes from `FSceneRenderer::PreVisibilityFrameSetup`. `r.TemporalAASamples` above 8 uses
a Halton (2, 3) sequence, but the engine caps it at 8, 4 and 2 phases below 70, 50 and 30
percent screen percentage. The jitter is added to the projection with +Y up.

## What the add-on changes

- **Render scale and jitter.** `r.ScreenPercentage` and `r.TemporalAASamples`
  (8 × (output / render)², 18 for Quality) are written directly into their console variable
  data each frame and restored when DLSS is turned off.
- **View capture.** A hook on `PreVisibilityFrameSetup` presents full screen percentage to the
  jitter code for that call only, so the sample cap does not apply, and records the view rect,
  output rect, jitter and camera cut. The render thread runs ahead of the thread executing
  Direct3D work, so the capture is appended to the RHI command list as a small command that
  executes immediately before that frame's TAA.
- **TAA replacement.** ReShade's `dispatch`, `draw`, `bind_pipeline` and `push_descriptors`
  events recognise the game's shaders. They are identified by their DXBC checksum when created
  (`init_pipeline`); the game creates each more than once, so every copy is recorded. All
  add-on passes run in a separate `ID3DDeviceContextState`, so the engine's cached bindings are
  untouched.
- **Motion.** The packed motion suits the TAA but not DLSS, which shows its quantization,
  dilation and zeroed pixels as warping. Beside `FDepthResolveCS`, while its inputs are bound,
  a compute shader repeats the resolve's motion maths per pixel in float: GBuffer velocity
  (UE4's `DecodeVelocityFromTexture`) or camera reprojection from depth, back to render pixels
  through the resolve's pixel to screen mapping. It also copies depth and writes the TAA's
  history rejection for moving pixels without object velocity as DLSS's current-colour bias
  mask.
- **Evaluation.** At the TAA dispatch a prepare shader copies the colour, and NGX evaluates
  DLSS with inverted depth and undilated render-resolution motion vectors (`MVLowRes`, in render
  pixels). At full resolution a compute shader then performs the
  TAA's composite into its output. When upscaling, the game's composite draw runs with a
  replacement pixel shader and the DLSS output in place of its history. Both composites read
  the game's own constant buffer, so vignette, grain, UI and HDR brightness match the original.
  Mode changes wait until the RHI reaches a view near the requested render size; queued
  views from the previous mode use game TAA instead of violating DLSS's input-size limits.
  The colour is already tone-mapped, with white at 1, so DLSS is handed a 1x1 exposure texture
  holding a constant 1 (pre-exposure and exposure scale 1 too). Bend's own scene exposure is
  applied in its lighting, before this, and would be the wrong number here. The texture is for
  add-ons that follow the evaluation: OptiScaler's DLSS 5 neural rendering takes its white point
  from it, and without one OptiScaler's Direct3D 11 bridge turns on DLSS's auto-exposure.
- **HDR.** The square-root encoding exceeds 1 in HDR, so the prepare shader squares the colour
  and DLSS runs with `IsHDR` on linear values. The composites then skip their squaring, apply
  the game's scene and UI brightness and encode PQ.
- **Sharpening.** The game's TAA sharpens the current frame (centre + 4 × (centre − 3×3 mean),
  weighted by local contrast), and DLSS has no sharpening of its own. With the Sharpening
  setting above 0 (default 33%, tuned on an LG C1 in HDR), both composites run AMD FidelityFX
  RCAS (FSR 1, MIT) on the DLSS output before vignette, grain and UI. The setting scales
  RCAS's negative lobe, so 100% is RCAS at 0 stops. Its noise filter is left out because grain
  is applied afterwards. RCAS works on the square-root encoding and limits itself to 0 to 1,
  so HDR, which exceeds 1, is sharpened as x / (1 + x) and mapped back.
- **Texture detail.** While rendering below output resolution, mipmapped pixel shader samplers
  that are filtered and wrap or mirror on U and V are swapped for copies with
  `log2(render / output)` plus the user's offset added to their LOD bias. These are the
  samplers Bend biases itself when rendering below output resolution on PS4 Pro
  (`r.Bend.Texture.NeoGlobalBias`). Point, clamped and comparison samplers belong to
  screen-space passes. D3D11 adds a sampler's bias to explicit mip reads too, so biasing them
  would, for example, make SSAO's wider samples read full-detail depth and darken the ground.
- **Ambient occlusion.** The deferred lighting shaders truncate their reconstruction weights
  to integers. Small interpolation errors can turn a nominal weight of 1 into 0; at foliage
  depth edges all other samples can be rejected, producing fully unoccluded pixels in a 2×2
  pattern before DLSS. Insert `round_ne` before this integer conversion in the 30 verified
  lighting permutations. The samples, directional decode, depth rejection, spatial filter
  and temporal accumulation are unchanged. At each lighting draw, require an `R32G32_UINT` AO
  view of the same size as the bound render target; other sizes use the original shader.
  Compare with the target rather than the viewport: the engine pads its buffers to a multiple
  of 4 pixels, so a 427×241 view renders into 428×244 textures.
  Original DXBC instructions and constants are retained; only the rounding instruction and
  the necessary container sizes, offsets and checksum change. No extracted game shaders are
  included in the distribution.
- **Indirect lighting (1.07).** 1.07's screen-space indirect lighting traces a new random ray per
  pixel every frame and leaves the noise to its TAA, which DLSS shows as boiling below Balanced.
  With the "Screen-space indirect lighting" setting off, the add-on skips its four passes while
  DLSS runs and clears the result the lighting reads to black, as the game does when SSIL is off.
  DIFFERENCES.md has the details.

Any frame the replacement cannot handle uses the game's TAA and logs why once. If the upscale
composite does not follow a replaced TAA, DLSS stops instead of showing a stale history.

## FSR upscaling

With `upscaler=fsr` AMD FSR takes DLSS's place at the TAA dispatch, with the same inputs, composites
and sharpening. AMD's runtime picks the version: FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 on
other GPUs, with the version in the overlay's status line. It runs on Direct3D 12 only.

- **Interop.** The colour, depth, motion, colour bias and output textures are created shared. A
  native Direct3D 12 device on the game's GPU opens them; Direct3D 11 signals a shared fence and
  flushes after the prepare pass, Direct3D 12 runs FSR on its own queue, and Direct3D 11 waits on
  the GPU before the composite reads the output.
- **Inputs.** FSR works best on linear colour, so the prepare pass squares the square-root
  encoding in SDR too and the composites treat FSR's output as linear, as for HDR. The colour bias
  mask is FSR's reactive mask. Depth is reversed and infinite; the near plane and vertical field of
  view come from the captured projection, and UE4's centimetres are 0.01 metres.
- **Modes.** AMD's ratios: Quality 1.5x, Balanced 1.7x, Performance 2x, Ultra Performance 3x,
  and Native AA in place of DLAA. The FSR context is created for the plan's render and output sizes
  and HDR, like the DLSS feature, and the upscaler not chosen frees its memory.

## Frame generation

DLSS Frame Generation runs through NVIDIA Streamline, which supports Direct3D 12 and Vulkan only.
With `frame_generation` set to `2x` to `6x` or `dynamic` in `DaysGoneDLSS.ini`, the game presents
through Direct3D 12 from its next start; changing the multiplier, or turning generation off, applies
at once.

- **Swap chain.** When the add-on loads it takes over the `CreateSwapChainForHwnd` slot of DXGI's
  factory function table, in front of ReShade's hook. The game's swap chain becomes a stand-in
  (`src/presenter.cpp`) whose back buffer is a texture Direct3D 11 shares with Direct3D 12. A
  native Direct3D 12 device on the same GPU makes its queue and the real swap chain through
  Streamline's proxies, so Streamline owns presentation. Each present signals a shared fence after
  the game's frame; Direct3D 12 copies the back buffer into the swap chain's current one, and
  Direct3D 11 waits for the copy before the next frame draws into the same texture. Calls that do
  not concern the back buffer pass on to the real swap chain. When Streamline does not start (an
  unsupported GPU, missing files), the game keeps its own swap chain.
- **Present counts.** Frame generation presents several times per game frame, so the real swap
  chain's counts are not the game's. The stand-in reports its own: `GetLastPresentCount` and
  `GetFrameStatistics` count the game's presents, with the time of the last one. 1.07 needs this
  (DIFFERENCES.md, Frame timing).
- **Overlay.** ReShade wraps only swap chains made on its own proxies, and frame generation presents
  from a queue of its own, so ReShade's runtime is created on the stand-in instead
  (`create_effect_runtime`). It draws into the back buffer before the copy and is recreated when
  the buffers resize or the colour space changes; the stand-in keeps the colour space where ReShade
  reads it.
- **Inputs.** The upscaler's depth and render-resolution motion vectors are created shared, and
  both composites write a second output: the frame before the UI, as the hudless colour. Each
  present tags the three through Direct3D 12 as `eOnlyValidNow`, so Streamline copies them in the
  command list that copies the back buffer, before Direct3D 11 reuses them. A frame without DLSS
  (game TAA, a fallback, no scene) turns generation off until DLSS runs again.
- **Camera.** The view hook also reads FViewMatrices: ProjMatrix (`+0xB60`), TranslatedViewMatrix
  (`+0x80` from it), ViewOrigin (`+0x160`) and the jitter PreVisibilityFrameSetup added to the
  projection (`+0x174`). They are trusted only when they rebuild the translated view-projection
  (`+0xC0`) the engine computed. Streamline gets the projection without the jitter, the camera's
  axes and position, and clip-to-previous-clip from the previous frame's camera, centred on the
  current one for precision.
- **Reflex.** Frame generation paces its frames with Reflex, whose markers must carry one frame
  number on every thread. `BeginRenderingViewFamily` increments `GFrameNumber` once per rendered
  frame on the game thread and stores it in the view family's FrameNumber (`+0x48`; FSceneView
  starts with its family). A hook on `FEngineLoop::Tick` sleeps and marks the simulation as
  `GFrameNumber + 1`; the view hook marks the render thread's start with the family's number,
  which travels with the view capture to the present. Without the Tick hook the present marks the
  next frame's sleep and simulation, after the game has read its input. The overlay shows Reflex's
  latency report, from the tick's start to the GPU finishing the frame; it stays empty when the
  markers' numbers disagree.

1.13's addresses follow; DIFFERENCES.md has 1.07's.

| Address (RVA) | Purpose |
| --- | --- |
| `0x20E6920` | `FSceneRenderer::PreVisibilityFrameSetup(RHICmdList)` |
| `0x19C0EA0` | RHI command memory page allocation |
| `0x1BF7F0` | `FEngineLoop::Tick`, hooked for Reflex with frame generation |
| `0x4A60C44` | `GFrameNumber` |
| `0x4ADCA50`, `0x4CD0620` | `r.ScreenPercentage` and `r.TemporalAASamples` data |
| `0x4CA41C8`, `0x4CA41E8`, `0x4CA4208` | TAA `_0000`, `_0010` (HDR) and `_0001` shader caches |
| `0x4CA41A8`, `0x4CA4198` | Composite `_00` and `_01` shader caches |
| `0x4CA4178`, `0x4CA4168` | HDR composite `_10` and `_11` shader caches |
| `0x4C46000`, `0x4C46010` | `FDepthResolveCS<false/true>` shader caches |

FViewInfo (0x2D50 bytes): view family `+0x0`, ViewRect `+0xB28`, UnscaledViewRect `+0xB38`,
ProjMatrix `+0xB60`, jittered `ViewMatrices.TranslatedViewProjectionMatrix` `+0xC20`, jitter
`+0x10F0`, screen percentage `+0x1744`, camera cut `+0x1755`, anti-aliasing method `+0x1760` (3 is
Bend TAA), view state `+0x1CE0`.

## FSR frame generation

With `frame_generation=fsr` the presenter makes AMD's frame generation swap chain in place of
Streamline's proxy, on the same stand-in route; Streamline is not loaded and there is no Reflex.
AMD's swap chain paces the frames and generates on the queue it presents from, so the presenter
gives it the copy queue: the copy, the hudless copy and FSR's preparation of depth and motion come
first on that queue, and Direct3D 11's next frame still waits only for the copy. FSR generates at
present, after Direct3D 11 may have drawn the next frame's hudless colour, so it reads a Direct3D 12
copy. Each frame configures generation with the game's frame number, which must rise by one, and
frames without upscaling turn it off. The generation context follows the back buffer's size, format
and HDR, and is recreated after a resize.

## Ray-tracing experiment

A path-tracing experiment, off by default and released as experimental. It runs on 1.13 only; on 1.07
the add-on never calls into it. Stage 1 traces the game's own geometry; stage 2 adds ray-traced sun
shadows, ambient occlusion, one bounce of sunlight, and shadows and a bounce for the game's local
lights to the game's lighting. **Ray tracing
(experimental)** in the overlay has a status line and a checkbox per feature that changes the image;
the debug view selector and statistics are under **Diagnostics**. F9 switches every feature off and back on for A/B comparison, with a note in
the corner for two seconds; the work goes on underneath, so switching back is instant.
The checkboxes are saved in `DaysGoneDLSS.ini` under the keys below and applied at startup.
`DaysGoneDLSS-diagnostics.ini` is read when it changes, with or without DLSS, overrides them without
saving them, and takes:

| Key | Default | Effect |
| --- | --- | --- |
| `ray_tracing_sun_shadows` | 0 | Traced sun shadows |
| `ray_tracing_replace_canopy_shadows` | 0 | With them, leave out the game's soft shadow under tree canopies |
| `ray_tracing_soft_sun_shadows` | 0 | With them, rays across the sun's or moon's disc, filtered |
| `ray_tracing_offscreen_geometry` | 1 | Also trace cached static draws and copies of the trees, so their shadows stay off screen |
| `ray_tracing_ambient_occlusion` | 0 | Traced ambient occlusion, combined with the game's (the darker of the two) |
| `ray_tracing_half_ambient_rays` | 0 | With it, each pixel traces two of its four rays and takes the other two from a neighbour |
| `ray_tracing_global_illumination` | 0 | One bounce of sunlight added to the game's lighting |
| `ray_tracing_light_shadows` | 0 | Traced shadows for the game's local lights, in place of their shadow maps |
| `ray_tracing_soft_light_shadows` | 0 | With them, rays across each light's size, filtered |
| `ray_tracing_light_bounce` | 0 | With them, one bounce of the local lights added to the game's lighting |
| `ray_tracing_view` | 0 | A debug view drawn over the image (below) |
| `ray_tracing_performance_overlay` | 0 | The cost table over the game, also with the overlay closed |
| `ray_tracing_measure_cost` | 0 | A new non-zero number starts a real-cost measurement (below) |

| View | Shows |
| --- | --- |
| 1 | The traced geometry, shaded by its normal (copies of trees flat) |
| 2 | Depth comparison: green where the traced hit matches the game's depth within 1%, red where it is nearer, blue where it is farther or missing |
| 3 | The traced sun light alone |
| 4 | Sun shadow comparison: the game's sun visibility in red, the traced one in green |
| 5 | Depth comparison of what sun rays see beyond the view: shadow casters, cached draws and tree copies |
| 6 | The traced and the game's ambient occlusion combined, as lighting reads it after the game's blur |
| 7 | Ambient occlusion comparison: the game's alone on the left, combined with the traced on the right |
| 8 | Off-screen check: red where a traced shadow would go once its caster leaves the view, blue where only geometry kept off screen shadows |
| 9 | The traced global illumination alone, as it is added to the scene |

- **Capture.** The view hook also records the jittered translated-world-to-clip matrix the base
  pass draws with. Bend's G-buffer is two packed targets (`R32G32_UINT`, `R32_UINT`); moving
  objects add `R16G16_UNORM` velocity. Of the G-buffer draws, opaque geometry either writes depth
  or matches the prepass (`EQUAL`). Decals blend; the sky dome is tested against the far plane
  without writing depth; full-screen passes into the G-buffer do not test depth. Those are skipped.
  Each captured draw is repeated before the game's draw with a stream-output geometry shader
  built from its vertex shader's bytecode, which writes the clip-space `SV_Position` of every
  triangle into one buffer. Draws with geometry or hull shaders, other topologies or no room
  left (4.2 million triangles, at most 2.1 million of them alpha-tested) are counted and skipped.
  The vertices are held three times, about 200 MB each: in that buffer, the shared positions
  texture and Direct3D 12's vertex buffer (below). So the room just covers the worst view seen,
  3.9 million triangles at Hot Springs with the caches off.
- **Positions.** A compute shader turns clip positions into world positions from clip x, y and w
  only. Some draws, foliage among them, narrow the viewport's depth range (to 0–0.0796) and scale
  clip z to match, so the full inverse would move their vertices about 12 times nearer along the
  view ray. Clip z still marks vertices at infinity (reversed-Z depth 0), which get a NaN x
  across their triangle so DXR treats it as inactive; any other NaN or infinity left the whole
  structure without hits.
- **Alpha-tested foliage.** Bend alpha-tests only in its depth prepass; the G-buffer pass then
  matches that depth without discarding, so masked meshes are recognised by their prepass draw's
  index range and instances. The prepass draw is captured instead, with the texture coordinates
  its alpha test samples. `src/alpha_test.cpp` slices the pixel shader's DXBC back from its
  `discard` to the input the samples read and makes a vertex shader that runs the game's own
  pixel shader over those coordinates. The material (pixel shader plus the textures it declares)
  is baked once into a 256² slot of an 8192² `R8` coverage atlas, which starts again when full.
  Most prepass tests also scale the opacity by one per-vertex value, a fade or a vertex colour
  channel; that value is captured too, and the bake sweeps it over 16 levels and keeps each
  texel's lowest passing level. The trace treats alpha-tested triangles as non-opaque and
  accepts a hit where the interpolated fade reaches the texel's level. Each vertex carries its
  material's slot beside its coordinates and fade, so the any-hit test, run for every candidate,
  finds the slot with one load instead of a search through the frame's draws. Stream output counts
  an element's components from its first, wherever it sits in its register. Pixel shaders whose
  discard reads no texture only dither whole meshes in and out between LODs, as most of Bend's
  trees do; their draws are captured as solid geometry. Others dither by an LOD fade-in and
  crossfade on top of the texture test (the conifer needles): the bake runs every combination of 0
  and 1 for the discard's inputs besides the coordinates and the fade, and keeps every texel some
  combination covers, since one of them is the mesh fully faded in. A slot covers a whole texture,
  so the bake samples through copies of the game's samplers clamped to the finest mip; at the mip
  a 256² slot would pick, thin needles blur into an opacity that passes everywhere and the cards
  shadow as solid rectangles. Sampled once per slot texel, the finest mip in turn misses needles a
  texel or two wide and the canopy lets too much sun through, so the bake runs at 1024² and a slot
  texel passes at the fourth-lowest level of its 16 samples. Some materials tile their mask across
  the mesh in the pixel shader (a chain-link fence's wire, many times over the mesh's 0 to 1): baked
  over 0 to 1, the wires filled every texel and the fence traced as a solid sheet. Where every
  sample behind the discard takes the coordinates times a scale plus an offset (`mul`/`mad` by
  constants or literals), the bake covers one tile instead, and each such draw's scale and offset
  are copied from its constants (`TransformDraws`/`TransformRows`) so Positions writes its
  coordinates in tiles.
- **Shadow casters.** The game culls geometry outside the view, so objects just off screen would
  stop shadowing it. Bend renders the sun's shadow into a 6912×2304 atlas of three 2304² maps: the
  first covers the whole shadow distance, the other two repeat nearer casters, and small objects
  are left out of the first. Draws into all three are captured like G-buffer draws, each draw once
  per frame: one already captured, on screen or in another map, is skipped (most are). The sun
  shadow resolve maps the view's translated world into the maps with rows 28 to 35 of its
  constants (rows 34 and 35 place the two near maps; row 35.z scales u into the atlas), so a
  caster's atlas position and depth give its world position. Those rows are copied on the GPU at
  the resolve, after the shadow passes and before the trace. The casters get their own
  bottom-level structure (instance mask 2): primary rays see only the view's geometry (mask 1),
  sun rays both. A caster of a static object the view drew this frame is skipped as well: the
  view's capture holds the object whole, at its own level of detail. So is one the geometry cache
  holds whole (below). Moving objects cannot be told apart in the shadow maps: their own constants
  are dynamic, so every one of them gets the same object key from the buffers they share. Their
  casters are always captured, since another one may be off screen. At the bridge near the start
  nearly all casters are the player's (about 290,000 vertices, 0.4 ms of build), so the skipped
  ones save little there.
- **Cached static geometry.** Casters stop at the shadow distance and leave out what the game
  culls by occlusion, so static draws the view has captured are also kept in a geometry cache,
  each with its own bottom-level structure (`PREFER_FAST_TRACE`) in rings of vertices,
  coordinates and structures that evict the oldest. A draw is identified by the static constant
  buffers its vertex shader declares (the primitive's own; per-draw constants are dynamic) and its
  instances; draws with a velocity target move and are never cached. Cached draws the view did not
  draw this frame are traced with mask 2. A static draw the cache already holds is not captured
  again: its entry stands in for it, traced with masks 1 and 2, and the rebuild leaves it out. The
  cache also stands in for the shadow casters of an object it holds whole: every draw the view last
  drew of it is cached and none has been evicted since. New entries never evict one standing in
  that frame, for the view or for casters; the ring moves past it. The trace reads a
  cached hit's vertices from the cache, from the instance's ID on, for its normal. The geometry
  was captured in translated world, so its instance moves by the camera's movement since: the
  view hook reads `PreViewTranslation` (`FViewInfo` + 0xCB4) and checks it against `ViewOrigin`
  (+ 0xCC0).
- **Foliage.** Bend draws trees and plants as instanced draws whose vertex shader reads each
  instance's transform (three rows, translation in w) from a static structured buffer at `t0`, by
  an index in the low 24 bits of a per-frame list at vertex buffer slot 0, and draws none of them
  into the shadow maps (they shadow through the soft canopy map below). Every fourth frame's lists
  are copied and read back, with four frames to land: a tree keeps every mesh it was drawn with in
  the latest of those frames that saw it (trunk, branches and leaves are separate draws, at the LOD
  of that frame). Each mesh's first instance in such a frame is cached once as its template, in a
  cache of its own. A mesh is its draw's shaders, buffers and index range and, when alpha-tested,
  the textures its alpha test declares: Bend draws the same vertices with many plants' textures
  (one with 35), and without them one plant's copies stood in for all (pines in place of every
  other tree). Trees within 80 m or seen in the last 150 read-backs (about 20 s) are placed as
  instances of their meshes' templates (mask 4), by M_tree·M_reference⁻¹ and the camera's
  movement. A tree is placed once, without the camera's position, and again only when a read-back
  gives it other meshes, its transforms are reloaded or one of its templates changes; each frame
  only adds the camera's position as the trace writes the instances. A tree neither near nor seen
  lately is dropped until it is seen again, so placing goes through the live trees, not every tree
  ever seen. A tree keeps up to eight sections. Reading back and placing ran every frame and took
  2.5 ms of the render thread at 4K in a forest, so the trees are now placed only at the first draw
  of a frame after a read-back or reload landed or the templates changed, and the frame after the
  read-back, not with it: a quiet frame does neither. The trace copies the result, less the copies
  of any template its own additions evicted (placed again the next frame). Sun rays test the copies
  from 30 cm plus 0.2% of the distance on, so a copy never shadows its own tree's leaves. Once a mesh's copy is ready, the view stops capturing its draws:
  the copies stand in for them, which saves most of the rebuild. The copies keep the pose the
  template was captured in, so their shadows do not sway with the wind.
- **Interop and trace.** Direct3D 11 cannot share buffers with Direct3D 12, and D3D12-created
  resources do not open in D3D11 on this driver, so Direct3D 11 creates every shared resource:
  textures with `SHARED | SHARED_NTHANDLE` and a shared fence. A D3D12 device on the same
  adapter, unwrapped from ReShade's proxy, copies the positions into a vertex buffer, rebuilds the
  view's and the casters' bottom-level structures and the top-level one (`PREFER_FAST_BUILD`),
  builds the cache's new entries, and traces with inline ray queries
  (`src/ray_tracing_trace.hlsl`, `cs_6_5`). The debug views' primary rays follow the inverse of
  the jittered matrix, so hits land on the rasterised pixels. Direct3D 11 waits on the GPU for the
  result. Building the view's structure earlier, at the first shadow map draw on a list of its own,
  saved nothing (0.03 to 0.09 ms more at the bridge): the GPU runs the two devices' work in turn,
  not side by side. The GPU idles while the CPU writes the trace's instances and records it (1 to
  2.7 ms a frame in the logs), so the foliage, which needs nothing the frame captures, is read back
  and placed at the frame's first draw instead, and the cache finds the entries it must keep once
  per frame, not per new entry. The capture memory is marked high priority for residency because
  the video memory manager demoted it to system memory at 4K, which made the copy and builds many
  times slower.
- **Sun shadows.** Twelve `FBendSceneDirectionalShadow...ScreenSpaceResolvePS` variants, known by
  checksum, resolve the sun's shadow into an `R8G8_UNORM` target before lighting. Its red channel
  holds the square root of the sun's visibility; lighting squares it and skips the sun at 0. Row
  20 of their constants is the shadow projection's depth axis, the sun direction. When lighting
  first reads the target, the frame is traced: a sun ray starts from each pixel of the game's
  depth, along its G-buffer normal, which also gives the N·L that leaves surfaces facing away to
  the lighting. The G-buffer normal is copied over the view once a frame and shared by every
  feature. Primary rays run for the debug views, and where the game's depth or normal cannot be
  read: there sun rays start from their hits, along the traced normal where the hit is the same
  surface. A full-screen pass then writes the lower of the game's and the traced visibility, so
  shadows only the game sees stay. The game's visibility multiplies its shadow maps by a soft tree
  shadow from a light-space map of the canopies (two cascades of depth layers and densities), the
  only array texture every variant reads. Replacing the canopy shadows, the add-on draws the
  resolve itself with zeros there, which leaves that term at 1, and puts the game's binding back.
  The same pass follows the moon at night. With soft shadows each sun ray aims at a point on a disc
  0.5° in radius (the sun and moon are about 0.27°; haze softens the rest), varying by pixel and
  frame, and the visibility is accumulated over 12 frames and filtered twice (the shared denoiser
  below) before it is written.
- **Local lights.** Bend draws each local light (`BendDeferredLightShaders`) as a full-screen
  triangle per lighting model, added to the scene colour, scissored to the light and
  stencil-masked to its volume, which a `TDeferredLightVS<true>` draw marks just before; both vertex
  shaders are known by checksum. The light's constants (position in world and inverse radius,
  colour, direction, spot angles, source radius and length) are the first rows of constant buffer
  4, or 5 for hair. Shadows are inline: a cube map for point lights, a 2D map read with gather4 for
  spots, and a baked 3D volume for stationary spots. With light shadows, up to 16 lights a frame
  draw into slots of their own (the scene colour's size and format) with their shadow maps
  replaced by lit stand-ins (depth textures other than the scene's, and 3D textures); their
  constants are copied on the GPU and the lighting constants' exposure row with them. The first
  draw or dispatch after the lights runs a second Direct3D 12 pass
  (`src/ray_tracing_lights.hlsl`): per pixel inside each slot's scissor, a ray toward the light
  stops 20 cm (or the source radius) short of it so its fixture does not shadow it. Lights the
  game draws unshadowed (fires, the lantern's three wide spots) get shadows too. Light rays leave
  out the sun shadow map's casters: moving objects are among them at that map's level of detail,
  a coarser copy around the object the view drew, which blotched a car under the flashlight. The
  pass writes the lights summed unshadowed and the share of them that reaches each pixel, and
  Direct3D 11 adds their product to the scene colour, so texture detail stays the game's. With
  soft shadows two rays a light aim across its size (the game's source radius, else 2.5% of a
  point light's reach or 5 cm for a spot), and the share is filtered over 6 frames. With bounce
  light a cosine-weighted ray per pixel finds a surface and adds up the lights there (colour,
  falloff, cone, N·L, a shadow ray each, the light at least 50 cm away), filtered over 16 frames
  and multiplied by the pixel's diffuse colour. The second hand-over costs about 0.3 ms; the rays
  for a few lights 0.03 to 0.06 ms, soft shadows and bounce about 2 ms at 4K.
- **Denoiser.** The sun's bounce, the lights' bounce and both soft shadows share one filter:
  reprojected accumulation that rejects history at another depth, then à-trous passes weighted by
  plane distance and normals. For the bounces, a pixel more than twice as bright as all its
  neighbours is first brought down to that, or a ray beside a fire shows as a firefly where foliage
  keeps little history.
- **Ambient occlusion.** Bend's screen-space AO is an `R32G32_UINT` target: eight cube-corner
  visibilities of 6 bits and the square root of depth in half precision. Of each opposite pair the
  lighting reads the corner on the normal's side, so `src/ray_tracing_ao.hlsl` traces those four,
  one cosine-weighted ray each, within 1 m plus 2.5% of the view distance. A hit at a fraction t
  of that radius leaves t² of the light: a linear falloff left the ground under a car most of it.
  Rays see the view's geometry, including the cached draws standing in for it, and the cached
  geometry off screen too, so a wall just outside the view still occludes where screen-space AO
  cannot. They see the trees' copies from 30 cm plus 0.2% of the view distance on, as sun rays do.
  The shadow casters are a coarser level of detail and are left out. With half the rays, each pixel
  traces two corners, in a checkerboard that changes every frame, and takes the other two from its
  horizontal or else vertical neighbour if that sees the same corners within 2% of its distance;
  otherwise the mean of its own two stands in. Tracing them instead would cost a whole wave of
  threads all four rays wherever one pixel needs them, which made it slower than full rays. The
  rays then take 0.47 instead of 0.73 ms at the bridge. The result is written in the game's
  packing, with the four untraced corners and pixels without a surface fully visible, and a pass
  keeps per corner the lower of it and the game's own (with the game's depth): the usual hybrid of
  traced and screen-space AO. The copies keep their template's wind and occlude only from 30 cm,
  so in the trace grass no longer darkens the ground under it; the game's AO gives that near
  contact, and the rays what the screen does not hold. Capturing the foliage of trees within 20 m
  in the view instead, for the rays, pulled whole grass draws (every visible instance, across the
  view) into the capture as alpha-tested geometry: it doubled the view's rebuilt triangles, and the
  real-cost measurement charged AO 14.5 ms, of which the rays and filter were 2.3 ms. The add-on
  runs the game's own `BlurAmbientCS` on the combined result and copies it over the game's target
  when lighting first reads it, so the lighting's temporal accumulation filters it as it does the
  game's. The lighting only scales its ambient terms by AO, and only where it is darker than the
  material's baked occlusion, so in sunlight the difference is subtle.
- **Global illumination.** One bounce of sunlight, added to the game's lighting. Bend lights the
  opaque scene with a full-screen draw per lighting model into an `R11G11B10_FLOAT` scene colour,
  each picked by the stencil's low 4 bits, blending off. Models 1 to 3 keep the square root of the
  base colour in 8 bits a channel of the first G-buffer target's first word (model 1 also its
  metalness in the second word's low byte); hair (7) and an emissive model pack 5:6:5. As lighting
  starts, a compute pass copies the diffuse colour of models 1 to 3 over the view for Direct3D 12
  (the normals are the copy every feature shares), and the lighting's sun colour (rows 33 and 140
  of constant buffer 2; it moves toward twice row 140 by 1.6e-5 per cm of height, the camera's in
  row 53 of buffer 1) and exposure (row 43) are copied. `src/ray_tracing_bounce.hlsl` sends one
  cosine-weighted ray per pixel from the game's surface, up to 50 m, through the view's geometry
  and, from 1 m plus 1% of the view distance on, the casters, cached draws and foliage instances:
  their copies of on-screen geometry would otherwise light a surface from itself. A sun ray tests
  the hit. The hit's diffuse colour is the G-buffer's where it projects onto a pixel at the same
  depth, 0.25 grey otherwise. Direct3D 11 reprojects last frame's result, rejecting depths more
  than 3% apart, accumulates up to 24 frames, filters with three passes of a 5×5 à-trous kernel
  weighted by plane distance and normals, and scales by the sun's colour, the exposure and 1/π. The
  first draw or dispatch after lighting that does not read the sun shadow adds it, times the
  pixel's diffuse colour, to the scene colour.
- **Costs.** Diagnostics shows what each feature costs per frame, averaged over about half a
  second, and **Show the costs over the game** keeps that table in a corner with the overlay
  closed. Direct3D 12's work is timed by its own timestamps; the add-on's Direct3D 11 passes
  (`src/pass_timer.cpp`) by timestamp queries read back a few frames later. The two devices take
  turns on the GPU, so the times add up. "Direct3D 12 hand-over" is how long Direct3D 11 waits
  beyond Direct3D 12's work. The sun rays run only for sun shadows and the debug views, the primary
  rays only for the debug views. The repeated draws run inside the game's passes and are not timed,
  so **Measure the real cost** times the frame with everything on, then leaves out the debug view,
  the light bounce, soft light shadows, light shadows, global illumination, ambient occlusion, soft
  sun shadows and sun shadows in turn (the scene is still captured and
  built), then pauses ray tracing as F9 does: 1.5 s to settle and 2.5 s timed per step. The
  difference between steps is what the left-out part really costs. The render thread's time
  preparing and submitting the trace is broken down into reading back and placing the foliage, the
  cache plans and instances, the Direct3D 11 flush, recording and submitting, the preparation
  before the flush and the rest. The foliage's part comes in bursts, a read-back every fourth frame
  and the placing the frame after, averaged with the quiet frames between. While ray tracing runs,
  `DaysGoneDLSS-performance.txt` beside the log holds the same numbers, rewritten every second,
  and the log has them every 600 traced frames.

Measured on an RTX 5080: the forest at Hot Springs (4K output, DLSS Ultra Performance) captures
about 3.9 million triangles from 1,600 of 1,900 G-buffer draws, about 730 of them alpha-tested
with about 490 materials. Rebuilding takes 5.6 to 6 ms of GPU time, the copy 0.4 ms and the
trace with sun rays 3.5 to 4.5 ms. The bridge near the start (1920×1080 windowed) adds about 290
shadow casters to 3,400 captured draws: 3.4 million triangles, 5.5 ms rebuild, 0.35 ms trace and
1.5 ms of CPU time repeating draws. Depth comparison is green on opaque geometry and foliage;
thin grass blades narrower than a coverage texel show as blue speckle, and dithered materials
such as hair as red.
