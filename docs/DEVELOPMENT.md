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
by commit and checked by SHA-256. The ray-tracing experiment's trace shader is compiled with the
Windows SDK's `dxc`, which the developer prompt puts on the PATH. The add-on is
written to `build/release/DaysGoneDLSS.addon64` together with `nvngx_dlss.dll`.

`cmake --install build/release --component addon --prefix <dir>` collects the add-on,
`nvngx_dlss.dll`, player documentation and licenses for distribution. Runtime files are at
the install root; documentation and licenses are in `DaysGoneDLSS/`.

## Package a release

From the repository root, after building and testing:

```
cpack --config build/release/CPackConfig.cmake -B build/release
```

This produces `build/release/DaysGoneDLSS-1.3.1.zip` and its `.sha256` checksum. CPack
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
| `tests/upscaler_tests.cpp` | Settings tests |
| `src/shader_bytecode.*` | Bounded DXBC parsing and checksum updates |
| `src/ambient_occlusion.*`, `tests/ambient_occlusion_tests.cpp` | AO reconstruction weight correction and WARP regression tests |
| `src/indirect_lighting.*` | Identifies 1.07's screen-space indirect lighting passes |
| `src/ray_tracing*`, `src/alpha_test.*`, `tests/ray_tracing_tests.cpp` | Ray-tracing experiment: geometry capture, alpha-test baking, DXR trace, sun shadows, global illumination and debug views |

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

1.13's addresses follow; DIFFERENCES.md has 1.07's.

| Address (RVA) | Purpose |
| --- | --- |
| `0x20E6920` | `FSceneRenderer::PreVisibilityFrameSetup(RHICmdList)` |
| `0x19C0EA0` | RHI command memory page allocation |
| `0x4ADCA50`, `0x4CD0620` | `r.ScreenPercentage` and `r.TemporalAASamples` data |
| `0x4CA41C8`, `0x4CA41E8`, `0x4CA4208` | TAA `_0000`, `_0010` (HDR) and `_0001` shader caches |
| `0x4CA41A8`, `0x4CA4198` | Composite `_00` and `_01` shader caches |
| `0x4CA4178`, `0x4CA4168` | HDR composite `_10` and `_11` shader caches |
| `0x4C46000`, `0x4C46010` | `FDepthResolveCS<false/true>` shader caches |

FViewInfo (0x2D50 bytes): ViewRect `+0xB28`, UnscaledViewRect `+0xB38`, jittered
`ViewMatrices.TranslatedViewProjectionMatrix` `+0xC20` (`ProjMatrix` is at `+0xB60`), jitter
`+0x10F0`, screen percentage `+0x1744`, camera cut `+0x1755`, anti-aliasing method `+0x1760` (3 is
Bend TAA), view state `+0x1CE0`.

## Ray-tracing experiment

A path-tracing experiment, off by default and not part of a release. It runs on 1.13 only; on 1.07
the add-on never calls into it. Stage 1 traces the game's own geometry; stage 2 adds ray-traced sun
shadows, ambient occlusion and one bounce of sunlight to the game's lighting. **Ray tracing
(experimental)** in the overlay has a status line and a checkbox per feature that changes the image;
the debug view selector and statistics are under **Diagnostics**. F9 switches every feature off and back on for A/B comparison, with a note in
the corner for two seconds; the work goes on underneath, so switching back is instant.
`DaysGoneDLSS-diagnostics.ini` is read when it changes, with or without DLSS, and takes:

| Key | Default | Effect |
| --- | --- | --- |
| `ray_tracing_sun_shadows` | 0 | Traced sun shadows |
| `ray_tracing_replace_canopy_shadows` | 0 | With them, leave out the game's soft shadow under tree canopies |
| `ray_tracing_offscreen_geometry` | 1 | Also trace cached static draws and copies of the trees, so their shadows stay off screen |
| `ray_tracing_ambient_occlusion` | 0 | Traced ambient occlusion in place of the game's |
| `ray_tracing_global_illumination` | 0 | One bounce of sunlight added to the game's lighting |
| `ray_tracing_view` | 0 | A debug view drawn over the image (below) |

| View | Shows |
| --- | --- |
| 1 | The traced geometry, shaded by its normal (copies of trees flat) |
| 2 | Depth comparison: green where the traced hit matches the game's depth within 1%, red where it is nearer, blue where it is farther or missing |
| 3 | The traced sun light alone |
| 4 | Sun shadow comparison: the game's sun visibility in red, the traced one in green |
| 5 | Depth comparison of what sun rays see beyond the view: shadow casters, cached draws and tree copies |
| 6 | The traced ambient occlusion as lighting reads it |
| 7 | Ambient occlusion comparison: the game's on the left, the traced on the right |
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
  left (5.6 million triangles) are counted and skipped.
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
  accepts a hit where the interpolated fade reaches the texel's level. Stream output counts an
  element's components from its first, wherever it sits in its register. Pixel shaders whose
  discard reads no texture only dither whole meshes in and out between LODs, as most of Bend's
  trees do; their draws are captured as solid geometry. Others dither by an LOD fade-in and
  crossfade on top of the texture test (the conifer needles): the bake runs every combination of 0
  and 1 for the discard's inputs besides the coordinates and the fade, and keeps every texel some
  combination covers, since one of them is the mesh fully faded in. A slot covers a whole texture,
  so the bake samples through copies of the game's samplers clamped to the finest mip; at the mip
  a 256² slot would pick, thin needles blur into an opacity that passes everywhere and the cards
  shadow as solid rectangles.
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
  sun rays both.
- **Cached static geometry.** Casters stop at the shadow distance and leave out what the game
  culls by occlusion, so static draws the view has captured are also kept in a geometry cache,
  each with its own bottom-level structure (`PREFER_FAST_TRACE`) in rings of vertices,
  coordinates and structures that evict the oldest. A draw is identified by the static constant
  buffers its vertex shader declares (the primitive's own; per-draw constants are dynamic) and its
  instances; draws with a velocity target move and are never cached. Cached draws the view did not
  draw this frame are traced with mask 2. A static draw the cache already holds is not captured
  again: its entry stands in for it, traced with masks 1 and 2, and the rebuild leaves it out. New
  entries never evict one standing in that frame; the ring moves past it. The trace reads a
  cached hit's vertices from the cache, from the instance's ID on, for its normal. The geometry
  was captured in translated world, so its instance moves by the camera's movement since: the
  view hook reads `PreViewTranslation` (`FViewInfo` + 0xCB4) and checks it against `ViewOrigin`
  (+ 0xCC0). Ambient occlusion traces only the view's own capture, so the view captures every
  draw while it runs.
- **Foliage.** Bend draws trees and plants as instanced draws whose vertex shader reads each
  instance's transform (three rows, translation in w) from a static structured buffer at `t0`, by
  an index in the low 24 bits of a per-frame list at vertex buffer slot 0, and draws none of them
  into the shadow maps (they shadow through the soft canopy map below). The lists are copied and
  read back over four frames: a tree keeps every mesh it was drawn with in the latest frame that
  saw it (trunk, branches and leaves are separate draws, at the LOD of that frame). Each mesh's
  first instance is cached once as its template, in a cache of its own. Trees within 80 m or seen
  in the last read-backs are placed as instances of their meshes' templates (mask 4), by
  M_tree·M_reference⁻¹ and the camera's movement. Sun rays test them from 30 cm plus 0.2% of the
  distance on, so a copy never shadows its own tree's leaves. Once a mesh's copy is ready, the
  view stops capturing its draws: the copies stand in for them, which saves most of the
  rebuild. The copies keep the pose the template was captured in, so their shadows do not sway
  with the wind. Ambient occlusion traces only the view's geometry, so the view keeps the foliage
  while it runs.
- **Interop and trace.** Direct3D 11 cannot share buffers with Direct3D 12, and D3D12-created
  resources do not open in D3D11 on this driver, so Direct3D 11 creates every shared resource:
  textures with `SHARED | SHARED_NTHANDLE` and a shared fence. A D3D12 device on the same
  adapter, unwrapped from ReShade's proxy, copies the positions into a vertex buffer, rebuilds the
  view's and the casters' bottom-level structures and the top-level one (`PREFER_FAST_BUILD`),
  builds the cache's new entries, and traces with inline ray queries
  (`src/ray_tracing_trace.hlsl`, `cs_6_5`). Rays follow the inverse of the jittered matrix, so
  hits land on the rasterised pixels. Direct3D 11 waits on the GPU for the result.
- **Sun shadows.** Twelve `FBendSceneDirectionalShadow...ScreenSpaceResolvePS` variants, known by
  checksum, resolve the sun's shadow into an `R8G8_UNORM` target before lighting. Its red channel
  holds the square root of the sun's visibility; lighting squares it and skips the sun at 0. Row
  20 of their constants is the shadow projection's depth axis, the sun direction. When lighting
  first reads the target, the frame is traced: a sun ray starts from each pixel of the game's
  depth, along the traced normal where the traced hit is the same surface. A full-screen pass
  then writes the lower of the game's and the traced visibility, so shadows only the game sees
  stay. The game's visibility multiplies its shadow maps by a soft tree shadow from a light-space
  map of the canopies (two cascades of depth layers and densities), the only array texture every
  variant reads. Replacing the canopy shadows, the add-on draws the resolve itself with zeros
  there, which leaves that term at 1, and puts the game's binding back.
- **Ambient occlusion.** Bend's screen-space AO is an `R32G32_UINT` target: eight cube-corner
  visibilities of 6 bits and the square root of depth in half precision. Of each opposite pair the
  lighting reads the corner on the normal's side, so `src/ray_tracing_ao.hlsl` traces those four,
  one cosine-weighted ray each through the view's geometry, within 1 m plus 2.5% of the view
  distance. A hit at a fraction t of that radius leaves t² of the light: a linear falloff left
  the ground under a car most of it. The result is written in the game's packing; the add-on runs
  the game's own `BlurAmbientCS` on it and copies it over the game's target when lighting first
  reads it, so the lighting's temporal accumulation filters it in place of the screen-space
  result. The lighting only scales its ambient terms by AO, and only where it is darker than the
  material's baked occlusion, so in sunlight the difference is subtle.
- **Global illumination.** One bounce of sunlight, added to the game's lighting. Bend lights the
  opaque scene with a full-screen draw per lighting model into an `R11G11B10_FLOAT` scene colour,
  each picked by the stencil's low 4 bits, blending off. Models 1 to 3 keep the square root of the
  base colour in 8 bits a channel of the first G-buffer target's first word (model 1 also its
  metalness in the second word's low byte); hair (7) and an emissive model pack 5:6:5. As lighting
  starts, a compute pass copies the normals and the diffuse colour of models 1 to 3 over the view
  for Direct3D 12, and the lighting's sun colour (rows 33 and 140 of constant buffer 2; it moves
  toward twice row 140 by 1.6e-5 per cm of height, the camera's in row 53 of buffer 1) and exposure
  (row 43) are copied. `src/ray_tracing_bounce.hlsl` sends one cosine-weighted ray per pixel from
  the game's surface, up to 50 m, through the view's geometry and, from 1 m plus 1% of the view
  distance on, the casters, cached draws and foliage instances: their copies of on-screen geometry
  would otherwise light a surface from itself. A sun ray tests the hit. The hit's diffuse colour is
  the G-buffer's where it projects onto a pixel at the same depth, 0.25 grey otherwise. Direct3D 11
  reprojects last frame's result, rejecting depths more than 3% apart, accumulates up to 24 frames,
  filters with three passes of a 5×5 à-trous kernel weighted by plane distance and normals, and
  scales by the sun's colour, the exposure and 1/π. The first draw or dispatch after lighting that
  does not read the sun shadow adds it, times the pixel's diffuse colour, to the scene colour.

Measured on an RTX 5080: the forest at Hot Springs (4K output, DLSS Ultra Performance) captures
about 3.9 million triangles from 1,600 of 1,900 G-buffer draws, about 730 of them alpha-tested
with about 490 materials. Rebuilding takes 5.6 to 6 ms of GPU time, the copy 0.4 ms and the
trace with sun rays 3.5 to 4.5 ms. The bridge near the start (1920×1080 windowed) adds about 290
shadow casters to 3,400 captured draws: 3.4 million triangles, 5.5 ms rebuild, 0.35 ms trace and
1.5 ms of CPU time repeating draws. Depth comparison is green on opaque geometry and foliage;
thin grass blades narrower than a coverage texel show as blue speckle, and dithered materials
such as hair as red.
