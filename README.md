# Days Gone DLSS

Adds NVIDIA DLSS Super Resolution, DLAA, Frame generation and Reflex, or AMD FSR upscaling and frame
generation, to Days Gone, with SDR and HDR support.

## Requirements

- Days Gone on Windows, Steam version **1.13** (build 19221447) or **1.07** (the `days_gone_classic`
  beta, build 10034136). The add-on detects which one is running and stays unloaded on others.
- [ReShade 6.8+ with full add-on support](https://reshade.me/#download).
- For DLSS: an **NVIDIA RTX GPU** and an up-to-date driver.
- For DLSS frame generation: an **RTX 40** (2x) or **RTX 50** (up to 6x and Dynamic), and Windows'
  hardware-accelerated GPU scheduling turned on.
- For FSR: any GPU with Direct3D 12. FSR 4 needs a **Radeon RX 7000 or 9000**; other GPUs get FSR 3.1.
  FSR's machine-learned frame generation needs a Radeon RX 9000; other GPUs get FSR 3.1's.

## Install

1. Install ReShade for `BendGame/Binaries/Win64/DaysGone.exe`, choosing **DirectX 10/11/12**.
   No effect packs are needed.
2. Extract the ZIP into `BendGame/Binaries/Win64`.
   `DaysGoneDLSS.addon64` and the included DLLs must sit beside `DaysGone.exe`.
3. Start the game, open the ReShade overlay, and find **Days Gone DLSS**.
   Choose **DLSS** or **FSR** as the upscaler, then **Quality** for upscaling or **DLAA** (FSR:
   **Native AA**) for native-resolution anti-aliasing. **Upscaling is off by default**.

## Use

Choose the upscaler, modes and presets in the overlay. Leave the other settings at their defaults to
start with. The overlay's first line shows which FSR version runs.

On 1.07, if foliage shimmers in Performance or lower modes, turn off **Screen-space indirect lighting**.
The upscaler controls the game's Render Scale while enabled; **Off** restores it.

**Frame generation**: DLSS 2x to 6x or Dynamic, which also turns on NVIDIA Reflex, or FSR 2x. Either
works with either upscaler and needs upscaling on. Turning it on, or switching between DLSS and FSR,
takes a restart.

**Ray tracing (experimental)**, on 1.13 only: sun shadows, ambient occlusion, global illumination
and light shadows, each with its own checkbox. Turning it on takes a restart. It costs a lot of
performance. F9 switches it off and on to compare with the game.

Settings are saved in `DaysGoneDLSS.ini` beside the add-on.

To **update**, close the game and replace the ZIP's files. Keep your INI to retain settings.

To **uninstall**, delete `DaysGoneDLSS.addon64`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`, the
`sl.*.dll` files and the `amd_fidelityfx_*.dll` files.
You can also remove its INI, log, and `DaysGoneDLSS` documentation folder.

## Problems

If the add-on is missing, check the install path, game version, and ReShade's full add-on
support. For DLSS or FSR errors, check the overlay status and your GPU driver.

Report issues with `DaysGoneDLSS.log` and `ReShade.log` from the game executable's folder (with
frame generation also `sl.log`), plus your GPU, driver, resolution, SDR/HDR setting, and steps to
reproduce.

## Technical reference

See [DEVELOPMENT.md](docs/DEVELOPMENT.md) for build instructions and rendering implementation details.

## License

Code and documentation: Bustanity, MIT. NVIDIA DLSS, NVIDIA Streamline, AMD FSR, MinHook, ReShade,
Dear ImGui, AMD FidelityFX (the sharpening filter), and Microsoft's DirectX Shader Compiler checksum
implementation retain their own licenses, included in the ZIP's
`DaysGoneDLSS` folder.
