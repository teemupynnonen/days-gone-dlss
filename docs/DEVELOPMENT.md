# Days Gone DLSS development

## Build

Requires Visual Studio 2022 (or Build Tools) with the C++ workload and CMake 3.25 or later.
From a x64 developer prompt:

```
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release
```

CMake downloads MinHook, the DLSS SDK v310.9.1 headers, loader and `nvngx_dlss.dll`, and the
ReShade 6.8.0 add-on headers, each pinned by commit and checked by SHA-256. The add-on is
written to `build/release/DaysGoneDLSS.addon64` together with `nvngx_dlss.dll`.

`cmake --install build/release --component addon --prefix <dir>` collects the add-on,
`nvngx_dlss.dll`, player documentation and licenses for distribution. Runtime files are at
the install root; documentation and licenses are in `DaysGoneDLSS/`.

## Package a release

From the repository root, after building and testing:

```
cpack --config build/release/CPackConfig.cmake -B build/release
```

This produces `build/release/DaysGoneDLSS-1.2.0.zip` and its `.sha256` checksum. CPack
packages only the `addon` component, so SDK headers, static libraries, tests, debug symbols,
and local settings are excluded. ReShade is installed separately by the player.

## Sources

| File | Purpose |
| --- | --- |
| `src/upscaler.cpp`, `src/upscaler.hpp` | Core: engine hooks, shader identification, DLSS evaluation and composite |
| `src/upscaler_shaders.hpp` | HLSL for the input preparation and the SDR/HDR composites |
| `src/upscaler_addon.cpp` | ReShade add-on: events, exports, settings location |
| `src/upscaler_menu.cpp` | Settings window in the ReShade overlay |
| `src/upscaler_settings.cpp`, `src/upscaler_settings.hpp` | Modes, render scales and `DaysGoneDLSS.ini` |
| `tests/upscaler_tests.cpp` | Settings tests |

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

Any frame the replacement cannot handle uses the game's TAA and logs why once. If the upscale
composite does not follow a replaced TAA, DLSS stops instead of showing a stale history.

| Address (RVA) | Purpose |
| --- | --- |
| `0x20E6920` | `FSceneRenderer::PreVisibilityFrameSetup(RHICmdList)` |
| `0x19C0EA0` | RHI command memory page allocation |
| `0x4ADCA50`, `0x4CD0620` | `r.ScreenPercentage` and `r.TemporalAASamples` data |
| `0x4CA41C8`, `0x4CA41E8`, `0x4CA4208` | TAA `_0000`, `_0010` (HDR) and `_0001` shader caches |
| `0x4CA41A8`, `0x4CA4198` | Composite `_00` and `_01` shader caches |
| `0x4CA4178`, `0x4CA4168` | HDR composite `_10` and `_11` shader caches |
| `0x4C46000`, `0x4C46010` | `FDepthResolveCS<false/true>` shader caches |

FViewInfo (0x2D50 bytes): ViewRect `+0xB28`, UnscaledViewRect `+0xB38`, jitter `+0x10F0`,
screen percentage `+0x1744`, camera cut `+0x1755`, anti-aliasing method `+0x1760` (3 is Bend
TAA), view state `+0x1CE0`.

## Status

Verified on an RTX 5080 (driver 616.92) in SDR: DLAA and Quality evaluate on every frame after
the first, with UI, vignette and grain intact. In HDR at 3840×2160, a 168,000-frame session
switching between DLAA and Quality and presets K, L and M used DLSS on all but 6 frames. A sweep of jitter and motion vector sign
combinations on a still scene kept the defaults; edge stability is about equal to the game's
TAA.
