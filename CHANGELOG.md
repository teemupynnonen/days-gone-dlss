# Days Gone DLSS changelog

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
