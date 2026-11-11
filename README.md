# fo4lipsync

F4SE plugin for Fallout 4 1.10.163 (old-gen), 1.10.980/984 (next-gen) and 1.11.x (Anniversary) that keeps lip sync on the voice at high frame rates and smooths mouth movement.

- **Sync**: each talking actor's lip time and voice timer are advanced by real elapsed time instead of the engine's frame time, so mouths no longer run ahead of or behind the audio (and lines stop overlapping) at high or uneven FPS.
- **Smoothing**: when a line starts, its lip data is lightly blurred over time so mouth shapes blend instead of snapping. Timing and length are unchanged.

Settings live in `Data/F4SE/Plugins/fo4lipsync.ini`. The log (`Documents/My Games/Fallout4/F4SE/fo4lipsync.log`) reports how fast the engine's lip clock ran compared to real time for the first few lines.

Requires F4SE and Address Library for F4SE Plugins.

Build: apply `patches/commonlibf4-1.11-runtime.patch` to `extern/CommonLibF4` (`git -C extern/CommonLibF4 apply ../../patches/commonlibf4-1.11-runtime.patch`), then `xmake f -m releasedbg -y && xmake`. CI does both on every push to main.

The patch fixes CommonLibF4 treating some 1.11.x builds (e.g. 1.11.137) as old-gen, which would load the wrong Address Library IDs.
