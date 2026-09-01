# CodexTex

CodexTex is a Windows-native OBJ texture projection tool. It loads an OBJ with
existing UVs and a PNG Base Color texture independently, captures the current
3D view, obtains an edited projection image from Codex ImageGen (or a local
PNG), masks the desired region, and bakes only the texture.

The OBJ, UVs, hidden-face state, camera, masks, and undo history are session
state. Only the PNG texture is ever saved.

## Requirements

- Windows 10 or later
- Visual Studio 2022 with Desktop development with C++
- CMake 3.28+
- vcpkg (the Visual Studio bundled copy is supported)
- Optional AI features: `codex` on `PATH`, signed in with ChatGPT, with the
  built-in `imagegen` skill enabled. CodexTex never starts login or falls back
  to an API key.

## Configure and build

From a Developer PowerShell:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc
```

If `VCPKG_ROOT` is not set, configure with the Visual Studio bundled toolchain:

```powershell
cmake -S . -B build -A x64 -T v143,version=14.44 `
  -DCMAKE_TOOLCHAIN_FILE="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The 14.44 toolset selection keeps the application ABI aligned with current
vcpkg binaries. Install that optional MSVC toolset from Visual Studio Installer
if it is not already present.

## Workflow

1. Open an OBJ and a PNG independently.
2. Optionally add any number of inference reference OBJ + PNG pairs. They use
   their authored world coordinates and appear only as viewport/ImageGen context.
3. Orbit to the desired view. In Face mode, click or lasso occluding triangles
   and hide them to expose recessed areas.
4. Capture the view. Visible reference sets are included in the ImageGen image,
   while the frozen bake depth contains only the primary OBJ.
5. Generate a projection with Codex ImageGen or open an external PNG with the
   same aspect ratio.
6. In Mask mode, optionally turn off `Show in viewport` for the reference sets,
   paint or lasso the projection area, adjust inward feathering,
   preview, then bake.
7. Repeat from another view, then save the PNG texture.

The viewport starts in an unlit Base Color mode so PNG texels are displayed
without lighting multiplication. `Neutral shading` is an optional display and
capture aid. The first-run docking layout reserves the main central area for the
3D viewport; `Fit primary view (F)` recenters and tightly frames the editable OBJ.
The UI loads a Windows Korean font for IME-composed Korean prompt input and keeps
the completed text as UTF-8 when it is sent to Codex.
CodexTex enables per-monitor DPI awareness before creating its window. UI fonts,
spacing, and the initial window size are rasterized at the monitor's native DPI,
and are rebuilt after a `WM_DPICHANGED` monitor transition instead of relying on
Windows bitmap scaling.

For overlapping or mirrored UV layouts, `Mirrored UV side` can exclude either
the OBJ's local `-X` or `+X` side from projection preview and baking. This stops
the opposite projection from overwriting the same UV region. Because both model
sides still sample shared texels, the resulting texture necessarily appears on
both sides; separate left/right detail requires non-overlapping UV islands.

Generated images are first produced by Codex's built-in ImageGen at its normal
Codex-managed location. CodexTex consumes `imageGeneration.savedPath` and
copies the selected image into its per-process session directory. That session
directory is removed on clean shutdown.

Mask feathering runs as a Direct3D 11 jump-flood compute pass. The binary lasso
or brush mask remains authoritative, so feathering only reduces alpha inward;
the frozen depth, visible-face set, and front-facing angle test are applied
again during the UV-space bake.

## Tests

The test target covers OBJ validation and UV-overlap diagnostics, PNG RGBA
round trips, mask/prompt parsing, an in-process WARP render/bake golden path,
and a test-only JSONL App Server executable for signed-out, missing-skill,
generation, structured-mask, and interrupt behavior. A real ImageGen E2E run
still requires a locally authenticated ChatGPT Codex installation.

## Scope

Version 1 supports one editable OBJ, one UV set, and one editable PNG Base Color texture,
plus read-only inference reference pairs. It does
not write OBJ/MTL/project files, unwrap UVs, or manage PBR texture sets. Inference
reference pairs are read-only session assets: they cannot be selected, hidden by
face tools, projected into, baked, or saved by CodexTex.
