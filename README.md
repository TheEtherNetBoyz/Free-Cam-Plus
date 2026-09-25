# Freecam+

Freecam+ is a standalone Dusklight mod that renders an independent Camera 2
window. It supports free-camera movement, optional always-on-top behavior,
Camera 2-only update-rate throttling, and an adjustable over-the-shoulder
camera that follows Link's world position.

## Features

- Independent free camera in a second window
- WASD movement, mouse look, vertical movement, and speed boost
- Camera 2 update rates: full speed, 30 FPS, 20 FPS, or 15 FPS
- Optional always-on-top window
- Optional over-the-shoulder Link follow mode
- Adjustable distance, side offset, height, aim height, and orbit angle
- Warp/resource-transition safety guards

## Build

Freecam+ uses the Dusklight Mod SDK. Clone or otherwise obtain a compatible
Dusklight source checkout, then run:

```sh
cmake -S . -B build -DDUSK_DIR=/path/to/Dusklight
cmake --build build --target secondary_camera_package --parallel 4
```

The package is written to:

```text
build/mods/secondary_camera.dusk
```

## GitHub Actions

The repository includes a GitHub Actions workflow that builds macOS arm64 and
Windows x86-64 packages automatically on pushes, pull requests, or manual
workflow runs. The finished `secondary_camera.dusk` files are available from
the workflow's **Freecam-plus-macos-arm64** and
**Freecam-plus-windows-x86_64** artifacts.

The workflow builds against the current Dusklight `main` branch. Newer
WindowService input and always-on-top functions are detected at runtime, so
the package uses enhanced controls on hosts that expose them. On Windows hosts
that still publish WindowService 1.0, Freecam+ falls back to native keyboard
polling and cursor recentering for the auxiliary window.

For a full-tree build, add:

```sh
-DDUSK_MOD_USE_FULL_TREE=ON
```

## Install

Copy `build/mods/secondary_camera.dusk` to Dusklight's `mods` directory and
restart Dusklight. On macOS, the default directory is:

```text
~/Library/Application Support/TwilitRealm/Dusklight/mods
```

Open the Freecam+ settings from Dusklight's mod menu. The over-the-shoulder
numeric controls are inside **Open Over-the-Shoulder Settings**.

## Standalone compatibility

The mod is built against Dusklight's public game/mod interfaces. Use a
Dusklight checkout with the compatible GameService ABI (major 2) and build the
package for the target platform and architecture. Pre-ABI-2 releases are not
supported.
