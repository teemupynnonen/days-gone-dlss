# Days Gone DLSS

Adds NVIDIA DLSS Super Resolution and DLAA to Days Gone, with SDR and HDR support.

## Requirements

- Days Gone on Windows, Steam version **1.13** (build 19221447) or **1.07** (the `days_gone_classic`
  beta, build 10034136). The add-on detects which one is running and stays unloaded on others.
- An **NVIDIA RTX GPU** and an up-to-date driver.
- [ReShade 6.8+ with full add-on support](https://reshade.me/#download).

## Install

1. Install ReShade for `BendGame/Binaries/Win64/DaysGone.exe`, choosing **DirectX 10/11/12**.
   No effect packs are needed.
2. Extract the ZIP into `BendGame/Binaries/Win64`.
   `DaysGoneDLSS.addon64` and the included `nvngx_dlss.dll` must sit beside `DaysGone.exe`.
3. Start the game, open the ReShade overlay, and find **Days Gone DLSS**.
   Choose **Quality** for upscaling or **DLAA** for native-resolution anti-aliasing.
   **DLSS is off by default**.

## Use

Choose modes and presets in the overlay. Leave the other settings at their defaults to
start with. On 1.07, if foliage shimmers in Performance or lower modes, turn off
**Screen-space indirect lighting**. DLSS controls the game's Render Scale while enabled; **Off** restores it.
Settings are saved in `DaysGoneDLSS.ini` beside the add-on.

To **update**, close the game and replace the ZIP's files. Keep your INI to retain settings.

To **uninstall**, delete `DaysGoneDLSS.addon64` and `nvngx_dlss.dll`.
You can also remove its INI, log, and `DaysGoneDLSS` documentation folder.

## Problems

If the add-on is missing, check the install path, game version, and ReShade's full add-on
support. For DLSS errors, check the overlay status and your NVIDIA driver.

Report issues with `DaysGoneDLSS.log` and `ReShade.log` from the game executable's folder,
plus your GPU, driver, resolution, SDR/HDR setting, and steps to reproduce.

## Technical reference

See [DEVELOPMENT.md](docs/DEVELOPMENT.md) for build instructions and rendering implementation details.

## License

Code and documentation: Bustanity, MIT. NVIDIA DLSS, MinHook, ReShade, Dear ImGui, and
AMD FidelityFX (the sharpening filter), and Microsoft's DirectX Shader Compiler checksum
implementation retain their own licenses, included in the ZIP's
`DaysGoneDLSS` folder.
