# Days Gone DLSS changelog

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
