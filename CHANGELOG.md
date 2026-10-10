# Days Gone DLSS changelog

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
