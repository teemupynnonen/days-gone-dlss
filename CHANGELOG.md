# Days Gone DLSS changelog

## Unreleased

- The add-on now starts on copies of 1.13 and 1.07 from outside Steam, such as DRM-free copies,
  where it failed with "initialization was not successful". It told the game version from the
  executable's header, which those copies change; it now goes by the game's code.
- When the game's executable is not a supported build, ReShade.log now says so and names it.

## 1.7.1

- With frame generation on, ReShade and its add-ons now see the game as Direct3D 11, as without it.
  RenoDX's DLSS 5 Neural Rendering add-on found no DLSS with frame generation on; it now runs on
  the game's DLSS.

## 1.7.0

- Ray-traced sun shadows now fall on skin (faces, hands), not only on clothes. Bend keeps skin's
  sun shadow in another channel of its shadow texture, which the traced shadows did not write.
- Fix fallen branches, some road pieces and other repeated static meshes losing their ray-traced
  shadows and ambient occlusion with **Off-screen objects and trees** on: one cached copy stood in
  for every copy of the mesh, so the rest went missing from the traced scene.
- With **Off-screen objects and trees** on, leaf litter, grass and other plants now shadow and
  occlude the ground right under them. Their stand-in copies only counted from 30 cm away, which
  keeps a leaf from shadowing itself; that distance now applies to foliage only.
- Hand DLSS an exposure texture (a constant 1: the colour it upscales is already tone-mapped).
  Add-ons that follow the evaluation can read it; OptiScaler's DLSS 5 neural rendering takes its
  white point from it instead of reporting that the game supplies no exposure, and OptiScaler no
  longer turns on DLSS's auto-exposure for lack of one.
- Ray tracing: less noise in the bounce light, most of all in dark rooms at night. The filter keeps
  the rare rays that find a small lit patch instead of discarding them as fireflies, filters
  harder where the light is still noisy, and the rays' directions come from blue noise. The new
  **Reuse bounce rays** option (ReSTIR GI) lets pixels share the bounce rays of their neighbours
  and earlier frames.
- Ray tracing: the moon lights the night again with ray-traced sun shadows on, and global
  illumination bounces its light. The moon's disc, about 207 km away, was traced as geometry, so
  every ray toward the moon hit the moon itself and the traced shadows removed all moonlight.
- Ray tracing only starts when one of its features is on as the game starts, so it costs nothing
  while unused. Turning ray tracing on now takes a restart.
- Fix memory the ray tracer kept holding over long sessions, and cut work it repeated every frame.
  Moving a slider no longer rewrites the settings file every frame.

## 1.6.0

- Add AMD FSR upscaling, chosen with the new **Upscaler** setting. It runs on any GPU: FSR 4 on
  Radeon RX 7000 and 9000, FSR 3.1 on the rest. The modes are AMD's, with Native AA in place of DLAA.
- Add FSR frame generation (2x) on any GPU, with either upscaler. It presents through AMD's swap
  chain; switching between DLSS and FSR frame generation takes a restart.

## 1.5.0

- Add NVIDIA DLSS Frame Generation: 2x to 6x, or Dynamic to reach the display's refresh rate.
  It turns on NVIDIA Reflex, and the overlay shows the latency. The game then presents through
  Direct3D 12 by way of NVIDIA Streamline; turning it on takes a restart. Needs an RTX 40 (2x) or
  RTX 50 GPU, Windows' hardware-accelerated GPU scheduling, and DLSS or DLAA on.
- Add experimental ray tracing on Days Gone 1.13: sun shadows, ambient occlusion, global
  illumination and shadows for the game's lamps, fires and flashlight. Each is a checkbox under
  **Ray tracing (experimental)**, off by default, with soft shadow, bounce light and half-rate
  options, saved with the other settings. F9 switches it off and on to compare with the game. It
  costs a lot of performance.
  Only 1.13 is supported, and there are no plans to support other versions.
- Diagnostics shows what each ray-tracing feature costs, and can measure it.

## 1.4.1

- Fix a white flash at every camera cut in cutscenes on Days Gone 1.13. The game's upscaling
  blended in a texture that only its own anti-aliasing updates, which does not run with DLSS.

## 1.4.0

- Support Days Gone 1.07, the version before the Remastered update (Steam beta
  `days_gone_classic`). The add-on detects the game version when it loads. On 1.07 its
  composites follow that version's vignette, film grain and HDR output.
- Add a "Screen-space indirect lighting" setting on 1.07. 1.07's indirect lighting is noisy and
  makes foliage and ground boil with DLSS in Performance and lower modes; turning it off renders as
  the game's lower settings do.
- Rename the diagnostics checkbox to "Correct ambient occlusion weights".
- Tidy the overlay: a coloured status line at the top, explanations moved into tooltips,
  settings greyed out while DLSS is off, a "Reset to defaults" button, and a clear error when
  the game's shaders are not the expected ones. Diagnostics stay in their collapsed section,
  with the game shader list folded into one line.

## 1.3.1

- Apply the foliage grid fix in windowed mode and at custom render scales. It previously
  turned off unless the render size was a multiple of 4 pixels.

## 1.3.0

- Fix the foliage grid at low render scales by correcting ambient-occlusion weights.
- Fix DLSS stopping when switching from Ultra Performance to Quality.

## 1.2.0

- Fix a heat-haze shimmer with DLSS, strongest around foliage: motion vectors are now computed in
  full precision for every pixel instead of decoded from the game's TAA format.
- Fix smearing at the screen edges when turning and in fast motion, where the game's format had no
  motion.
- Keep DLSS from reusing history on moving objects the game draws without motion, as the game's own
  anti-aliasing does, such as the player's body in a first-person camera mod.
- Add a Current colour bias debug view.

## 1.1.0

- Fix smearing on movement in Quality and lower modes: DLSS now treats the game's motion vectors
  as render-resolution vectors.
- Fix dark patches on the ground from Balanced down: the texture detail bias no longer reaches
  the game's ambient occlusion and other screen-space effects.
- Add a Sharpening slider using AMD FidelityFX RCAS, 33% by default, to match the crispness of
  the game's own anti-aliasing.
- Keep the DLSS history while settings sliders move.

## 1.0.0

Initial release, prepared for publication.

- Replace the game's temporal anti-aliasing with NVIDIA DLSS Super Resolution or DLAA.
- Choose DLAA, Quality, Balanced, Performance, Ultra Performance, or a custom render scale.
- Support SDR and HDR output while preserving the game's UI, vignette, and film grain.
- Configure DLSS presets and texture detail bias through the ReShade overlay.
- Save settings beside the add-on and provide diagnostic logging.
- Target the Steam release of Days Gone, build 19221447, on NVIDIA RTX GPUs.
