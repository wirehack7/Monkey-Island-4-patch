# Monkey-Island-4-patch

A `dinput.dll` wrapper that makes *Escape from Monkey Island* (`Monkey4.exe`) playable on Windows 10/11 with current graphics cards, using the game's OpenGL renderer.

## What it fixes

- The game no longer switches the display mode. It runs in a borderless window covering the monitor instead.
- The game renders at whatever resolution you pick in its options (640x480, 800x600, 1024x768, ...). The image is scaled to your monitor with the original aspect ratio and black bars on the sides.
- Bink cutscenes are drawn through OpenGL instead of GDI, so they show up in the scaled window.

## Install

1. Download `dinput.dll` from the [latest release](../../releases/latest).
2. Copy it into the game folder, next to `Monkey4.exe`.
3. Start the game in OpenGL mode: `Monkey4.exe -gl`.

To uninstall, delete `dinput.dll`.

The wrapper writes `emifix.log` next to `Monkey4.exe`. Include it if you open an issue.

## Build

Requires Visual Studio 2022 (or the Build Tools) with the C++ workload.

```
build.bat
build\test.exe
```

The output is `build\dinput.dll` (32-bit). `build.bat` uses an already-set-up developer prompt if there is one and otherwise finds Visual Studio through `vswhere`.

## How it works

The game loads `dinput.dll` from its own folder first. The wrapper passes `DirectInputCreateA` on to the system `dinput.dll` and, when loaded, patches the game's import table:

| Function | Change |
| --- | --- |
| `ChangeDisplaySettingsA` | Ignored. The requested resolution is recorded. |
| `CreateWindowExA`, `MoveWindow` | The fullscreen window is sized to the monitor. |
| `wglMakeCurrent` | Creates an offscreen framebuffer at the game's resolution. |
| `SwapBuffers` | Scales the framebuffer onto the window, then swaps. |
| `glReadBuffer` | Redirected to the offscreen framebuffer. |
| `BinkCopyToBuffer`, `BinkBufferBlit` | Movie frames are uploaded as a texture instead of being blitted with GDI. |

Set the environment variable `EMIFIX_DUMP` to an existing folder to save a frame every now and then as BMP files for debugging.

## Releases

Pushing a tag like `v1.0.0` builds the DLL on GitHub Actions and publishes it as a release asset. Pull requests to `main` are built and tested the same way.

## License

MIT
