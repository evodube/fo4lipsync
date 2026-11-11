# fo4lipsync

F4SE plugin for Fallout 4 old-gen (1.10.163) that keeps lip sync on the voice at high frame rates and smooths mouth movement.

- **Sync**: each talking actor's lip time and voice timer are advanced by real elapsed time instead of the engine's frame time, so mouths no longer run ahead of or behind the audio (and lines stop overlapping) at high or uneven FPS.
- **Smoothing**: when a line starts, its lip data is lightly blurred over time so mouth shapes blend instead of snapping. Timing and length are unchanged.

Settings live in `Data/F4SE/Plugins/fo4lipsync.ini`. The log (`Documents/My Games/Fallout4/F4SE/fo4lipsync.log`) reports how fast the engine's lip clock ran compared to real time for the first few lines.

Requires F4SE and Address Library for F4SE Plugins.

Build: `xmake f -m releasedbg -y && xmake` (CI builds on every push to main).
