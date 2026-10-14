# Days Gone 1.07 and 1.13

What differs between the two supported builds and how the add-on handles it. Anything the add-on
depends on that is not listed here is identical.

1.13 is the current Steam release; 1.07 is the last version before the Remastered update (Steam
beta `days_gone_classic`). Both run Unreal 4.11 with Bend's renderer on Direct3D 11. The add-on
identifies the build from the executable's PE header (`Builds[]` in `src/upscaler.cpp`) and stays
unloaded on any other executable.

| Version | Steam build | Branch | TimeDateStamp | SizeOfImage | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| 1.13 | 19221447 | default | `0x68754D46` | `0x93F0000` | `17c2fca0…7730b3` |
| 1.07 | 10034136 | `days_gone_classic` | `0x60EDC8B2` | `0x64B6000` | `367af503…f3b689` |

## Executable

Only addresses and offsets changed; the code behind them is the same.

| RVA | 1.13 | 1.07 |
| --- | --- | --- |
| `FSceneRenderer::PreVisibilityFrameSetup` | `0x20E6920` | `0x1E3B9F0` |
| RHI command page allocation | `0x19C0EA0` | `0x1771BB0` |
| `r.ScreenPercentage` data | `0x4ADCA50` | `0x428B180` |
| `r.TemporalAASamples` data | `0x4CD0620` | `0x4480F10` |
| TAA `_0000`, `_0010`, `_0001` caches | `0x4CA41C8`, `0x4CA41E8`, `0x4CA4208` | `0x4455ED8`, `0x4455EF8`, `0x4455F18` |
| Composite `_00`, `_01`, `_10`, `_11` caches | `0x4CA41A8`, `0x4CA4198`, `0x4CA4178`, `0x4CA4168` | `0x4455EB8`, `0x4455EA8`, `0x4455E88`, `0x4455E78` |
| `FDepthResolveCS<false/true>` caches | `0x4C46000`, `0x4C46010` | `0x43F95B0`, `0x43F95C0` |
| `"UnrealEngine4"` mutex name | `0x3E23498` | `0x380A7E8` |

The two hooked functions have different prologues; each build carries its own bytes to verify.

FViewInfo is 0xAE0 bytes smaller in 1.07, and every field the add-on reads moved by that amount:

| FViewInfo | 1.13 | 1.07 |
| --- | --- | --- |
| Size | `0x2D50` | `0x2270` |
| ViewRect | `0xB28` | `0x48` |
| UnscaledViewRect | `0xB38` | `0x58` |
| TemporalJitterPixels | `0x10F0` | `0x610` |
| Screen percentage | `0x1744` | `0xC64` |
| bCameraCut | `0x1755` | `0xC75` |
| Anti-aliasing method | `0x1760` | `0xC80` |
| ViewState | `0x1CE0` | `0x1200` |

Unchanged: the renderer's views array, the RHI command list layout, the native shader offset in
Bend's shader objects, the jitter sequence and its phase caps, TAA variant selection and dispatch
size, and the TAA running after the UI pass.

## Shaders

1.07 loads its global shaders from the `_P` patch pak; the base pak's older copy is unused. Each
build has its own checksums for the nine shaders the add-on recognises, except
`TBendSMAATemporalAACS_T1X_0001`, which is identical. The HLSL is compiled with `GAME_VERSION` 107
or 113 to match.

**Depth resolve.** Only the row of its dither's frame counter moved (`cb3[13].y` → `cb3[12].y`).
The motion maths, velocity encoding and packed format are identical.

**Full-resolution TAA (DLAA).** Identical except its tail:

- 1.13 applies film grain twice (before decoding and after the vignette); 1.07 only before decoding.
- 1.13's HDR output is ST 2084 PQ after a soft toe. 1.07 uses a log fit,
  `saturate(16384x) · (saturate(0.0765512 · log2(0.5x + 0.00263665) + 0.65582) + saturate(0.14142 − 0.35355x)²)`,
  which is visibly darker in shadows.

**Upscale composite** (`TBendSMAABufferCopyPS`):

| | 1.13 | 1.07 |
| --- | --- | --- |
| Vignette tint, shape, edge | rows 2, 3, 4 | rows 0, 1, 2 |
| HDR scene and UI brightness | row 5 | row 3 |
| Film grain | after the vignette | none |
| Auxiliary history blend | when enabled by row 5 | none |
| `_01`/`_11` history filter | 5-tap sharpen | 6-tap hexagon blur, uv scaled about the centre by 1 − 4a |
| HDR encoding | PQ | log fit |
| Variant while upscaling | `_00`/`_10` at 95–110 %, else `_01`/`_11` | `_01`/`_11` |

`a` = 1 / history size − 1 / output size, from the two rows after the HDR brightness. The add-on
evaluates 1.07's vignette at the same scaled uv but does not scale the image itself; DLSS replaces
the game's filter, as it does 1.13's sharpening.

**Ambient occlusion.** 1.07 renders AO at half resolution with the same settings that give full
resolution in 1.13: it uses the checkerboard permutation of `FComputeAmbientCS` (1.13's `<true>`),
whose source pixel in each 2×2 block cycles with the frame counter. The add-on's AO weight fix
requires full-resolution AO, so it does not apply on 1.07. Extending it to half resolution made no
measurable difference.

## Screen-space indirect lighting

Below Balanced, 1.07 foliage and ground boil under DLSS, even on a still camera with jitter off.
Freezing passes showed `FBendScreenSpaceILCS` as the cause; AO, shadows, SSR and volumetrics are
not.

| | 1.07 | 1.13 |
| --- | --- | --- |
| Passes | `<0>` gather, `<3>` and `<4>` blurs, `<2>` downsample after lighting | `DepthCopy`, `Gather`, `MipGen`, `Copy`, `Temporal`, `Downsample` |
| Ray noise | Pixel hash × frame counter: white noise every frame | Interleaved gradient noise with a per-frame offset |
| History | 0.85 · exp(−8192 · d²) inside the vertical blur | (0.9 − 0.1 · alpha) · exp(−8 · d²) in its own pass |

1.07 drops history at the slightest motion and relies on its TAA to average the rest; DLSS does
not denoise. The add-on's 1.07 setting **Screen-space indirect lighting** (default on) turns it off
while DLSS runs: the four passes are skipped and the result the lighting reads is cleared to black,
which is what the game binds below its highest settings.

Tried and dropped: accumulating the gather with the add-on's motion vectors, 1.13's history weights,
and low-discrepancy rays all reduced the boiling but left drifting noise or trailing. Porting 1.13's
passes is possible (same G-buffer encodings, view buffer and output hook; the frame buffer gains one
row) but would mean feeding six shaders from 1.07's resources.
