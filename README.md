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
4. Enter a prompt and choose `Generate from current view`. CodexTex immediately
   captures the cyan square and opens a locked projection workspace tab; there is
   no separate capture step. `External PNG from current view` creates the same tab
   without an AI request.
5. Return to `Main Viewport` while ImageGen runs and create more projection tabs
   from other camera positions. Each tab owns its frozen camera, depth, hidden-face
   snapshot, mask, AI job, and optional reference visibility.
6. In any completed projection tab, paint or lasso the projection area, adjust
   inward feathering, preview, then bake.
7. All projection tabs bake into the same live working texture and see changes
   made by other tabs. Undo/redo is shared. Repeat as needed, then save the PNG.

The viewport starts in an unlit Base Color mode so PNG texels are displayed
without lighting multiplication. `Neutral shading` is an optional display and
capture aid. The first-run docking layout reserves the main central area for the
3D viewport; `Fit primary view (F)` recenters and tightly frames the editable OBJ.
Viewport backgrounds use a clearly distinguishable solid blue-gray instead of
near-black. `Background color` in `Viewport display` changes the solid color for
every viewport and for subsequent ImageGen captures without affecting mesh or
reference textures.
Every viewport renders the same live working texture by default. The main viewport
alone has an `원본 텍스처 렌더링` checkbox in a rounded top-right viewport overlay;
it temporarily displays the PNG as originally loaded without changing the shared
working texture or affecting projection tabs.
Projection painting tabs use a matching top-right overlay with three per-tab view
modes: `작업` shows the shared working texture plus the current masked projection
preview, `원본` shows only the PNG as loaded, and `생성 전체` ignores the mask and
fully previews the generated image over the projectable primary-mesh surface.
`생성 전체` remains disabled until a projection image is available, and generated
pixels are never applied to inference reference assets in this preview.
The viewport crop frame shows the exact centered square sent to ImageGen. Generated
images and external projection PNGs must also be square; preview, mask editing,
and UV baking are clipped and mapped back to that frame without stretching.
The UI loads a Windows Korean font for IME-composed Korean prompt input and keeps
the completed text as UTF-8 when it is sent to Codex.

The main viewport offers model and reasoning-effort selectors populated from the
App Server's `model/list` response. With no settings file, the built-in selection
is `gpt-5.6-sol` with `medium` effort. Selection changes remain pending in memory
and are written immediately before the next generation request to
`CodexTex.settings.json` beside `CodexTex.exe`; startup reads that file when it
exists. Each projection tab records the exact model and effort used for its AI
work, and both values are sent explicitly with `turn/start` so Codex session
defaults cannot silently replace them.
CodexTex enables per-monitor DPI awareness before creating its window. UI fonts,
spacing, and the initial window size are rasterized at the monitor's native DPI,
and are rebuilt after a `WM_DPICHANGED` monitor transition instead of relying on
Windows bitmap scaling.

Projection tabs show the fixed OBJ, Base Color, triangle count, capture size,
and hidden-face snapshot in a read-only source panel. Primary OBJ/texture loading
is available only from `Main Viewport`; inference reference OBJ + PNG pairs can
still be added, removed, or toggled from either context.

For overlapping or mirrored UV layouts, `Mirrored UV side` can exclude either
the OBJ's local `-X` or `+X` side from projection preview and baking. This stops
the opposite projection from overwriting the same UV region. Because both model
sides still sample shared texels, the resulting texture necessarily appears on
both sides; separate left/right detail requires non-overlapping UV islands.

Generated images are first produced by Codex's built-in ImageGen at its normal
Codex-managed location. CodexTex consumes `imageGeneration.savedPath` and
copies the selected image into its per-process session directory. That session
directory is removed on clean shutdown.

The `Session Temp` panel lists files currently stored in that directory. PNG
captures and generated images can be previewed directly; other files expose a
bounded text/binary preview. Files can be deleted individually, or the whole
session directory can be cleared without removing the directory itself. Deleting
a file used by a projection workspace closes that tab and cancels its AI task;
clearing everything closes all projection tabs after an explicit confirmation.

CodexTex launches the first Codex App Server it can resolve from a native
`codex.exe`, an npm `codex.cmd` shim, or the versioned Codex desktop installation
under `%LOCALAPPDATA%`. `Retry Codex detection` reruns the executable and protocol
checks after the CLI, PATH, login, or skill availability changes.

Mask feathering runs as a Direct3D 11 jump-flood compute pass. The binary lasso
or brush mask remains authoritative, so feathering only reduces alpha inward;
the frozen depth, visible-face set, and front-facing angle test are applied
again during the UV-space bake.

## Tests

The test target covers OBJ validation and UV-overlap diagnostics, PNG RGBA
round trips, mask/prompt parsing, an in-process WARP render/bake golden path,
and a test-only JSONL App Server executable for signed-out, missing-skill,
generation, concurrent per-tab job routing, structured-mask, and interrupt behavior. A real ImageGen E2E run
still requires a locally authenticated ChatGPT Codex installation.

## Scope

Version 1 supports one editable OBJ, one UV set, and one editable PNG Base Color texture,
plus read-only inference reference pairs. It does
not write OBJ/MTL/project files, unwrap UVs, or manage PBR texture sets. Inference
reference pairs are read-only session assets: they cannot be selected, hidden by
face tools, projected into, baked, or saved by CodexTex.
