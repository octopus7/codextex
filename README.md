# CodexTex

CodexTex is a Windows-native texture projection tool for OBJ, FBX, and GLB
models. OBJ uses a separately loaded PNG; FBX and GLB import static material
surfaces and their Base Color images. Choose one material to edit, capture the
current 3D view, obtain an edited projection image from Codex ImageGen (or a
local PNG), mask the desired region, and bake into the working texture.

Geometry, UVs, material selection, hidden faces, camera, masks, and undo history
are session state. The edited result is saved as PNG. Source model containers
and their embedded images are never rewritten.

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

1. Open a model. For OBJ, load its PNG independently. For FBX or GLB, choose the
   Base Color material to edit when the model has several surfaces. A single
   editable surface opens directly. Use `Choose Base Color PNG` to relink a
   missing authored image; UV and material restrictions still apply.
2. Optionally add inference references: OBJ + PNG pairs or FBX/GLB models with
   their material images. They appear only as viewport/ImageGen context. Other
   materials in the editable model also remain visible as context, independently
   of the inference-reference visibility switch.
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

`Editing material` changes the active material from the main viewport. Before
replacing a dirty working texture through model, material, or image loading,
CodexTex offers the existing save/discard/cancel confirmation. A successful model
or material change resets projection tabs, face selection, and hidden faces.
Replacing the working texture also resets texture undo history; opening only an
OBJ preserves the independently loaded texture and its history. Failed imports or GPU resource preparation preserve the
current session. Materials sharing the same image also display its edited texels;
they do not receive independent texture copies for painting.

FBX/GLB parsing runs in the background. `Cancel import` discards the pending
result; the application waits for the resource-limited import to finish cleanup
before starting another import. Cancellation does not interrupt the parser
immediately. A recent input records the model path, material index and name, and
an optional external PNG path. Older OBJ/PNG recent-input settings remain valid.
If the recorded material has changed, the material picker opens again.

Texture history retains at most eight snapshots across undo and redo, within a
256 MiB RGBA budget. Failed GPU updates leave history and the save point intact.
Returning to the saved state clears the unsaved marker.

The viewport starts in an unlit Base Color mode so PNG texels are displayed
without lighting multiplication. `Neutral shading` is an optional display and
capture aid. The first-run docking layout reserves the main central area for the
3D viewport; `Fit primary view (F)` recenters and tightly frames the editable surface.
Viewport backgrounds use a clearly distinguishable solid blue-gray instead of
near-black. `Background color` in `Viewport display` changes the solid color for
every viewport and for subsequent ImageGen captures without affecting mesh or
reference textures.
Every viewport renders the same live working texture by default. The main viewport
alone has a localized `Original texture` checkbox in a rounded top-right viewport overlay;
it temporarily displays the PNG as originally loaded without changing the shared
working texture or affecting projection tabs.
Projection painting tabs use a matching top-right overlay with three per-tab view
modes: `Working` shows the shared working texture plus the current masked projection
preview, `Original` shows only the PNG as loaded, and `Generated Image` ignores the mask and
fully previews the generated image over the projectable primary-mesh surface.
`Generated Image` remains disabled until a projection image is available, and generated
pixels are never applied to inference reference assets in this preview.
The viewport crop frame shows the exact centered square sent to ImageGen. Pressing
Generate renders that view again into a dedicated 1024x1024 GPU target, independent
of the on-screen viewport size or DPI. Generated images and external projection PNGs
must also be square; preview, mask editing, and UV baking map back to that frame
without stretching.
The UI supports English, Japanese, and Korean. On first launch it follows the
Windows user locale and falls back to English for unsupported locales. A choice
under `Settings > Language` is saved immediately and takes precedence on later
launches; its entries always remain `English`, `日本語`, and `한국어` in their own
scripts. Japanese and Korean Windows fonts are merged into the UI atlas so every
entry remains legible, and IME-composed prompt text stays UTF-8 when sent to Codex.

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

Projection tabs show the fixed model, Base Color, triangle count, 1024x1024 offline
capture size, and hidden-face snapshot in a read-only source panel. Primary model,
material, and texture loading is available only from `Main Viewport`; inference
references can still be added, removed, or toggled from either context.

Ctrl+O opens a model, Ctrl+T opens a PNG texture, Ctrl+S saves the working texture,
and Ctrl+Z/Y undo and redo texture changes. Shortcuts follow the menu's enabled
state; opening primary assets is limited to the main viewport. Text fields keep
their own Ctrl+Z/Y behavior, and holding a shortcut does not repeat the action.

For overlapping or mirrored UV layouts, `Mirrored UV side` can exclude either
side of the active mesh's X midpoint from projection preview and baking. For
FBX/GLB, this is the selected material's merged bounds center in the imported
right-handed Y-up meter coordinates; it does not recover individual source-node
local axes. OBJ uses its authored, unitless coordinates. This stops
the opposite projection from overwriting the same UV region. Because both model
sides still sample shared texels, the resulting texture necessarily appears on
both sides; separate left/right detail requires non-overlapping UV islands.

Generated images are first produced by Codex's built-in ImageGen at its normal
Codex-managed location. CodexTex consumes `imageGeneration.savedPath`, then archives
the PNG under `%LocalAppData%/CodexTex/Generations` together with JSON metadata for
the prompt, model, camera, frozen capture, hidden faces, and reference assets.
Each projection job uses an isolated temporary subdirectory which is removed as
soon as its permanent image and metadata are safely finalized.

The `Session Temp` panel lists files currently stored in that directory. PNG
captures and generated images can be previewed directly; other files expose a
bounded text/binary preview. Files can be deleted individually, or the whole
session directory can be cleared without removing the directory itself. Deleting
a file used by a projection workspace closes that tab and cancels its AI task;
clearing everything closes all projection tabs after an explicit confirmation.
New projections are disabled until that cleanup reports completion or failure.
If permanent archiving fails, recovery files remain in the session folder even
after closing tabs or exiting the app. An explicit successful bulk deletion
clears that recovery record.

CodexTex launches the first Codex App Server it can resolve from a native
`codex.exe`, an npm `codex.cmd` shim, or the versioned Codex desktop installation
under `%LOCALAPPDATA%`. `Retry Codex detection` reruns the executable and protocol
checks after the CLI, PATH, login, or skill availability changes.

Mask feathering runs as a Direct3D 11 jump-flood compute pass. The binary lasso
or brush mask remains authoritative, so feathering only reduces alpha inward;
the frozen depth, visible-face set, and front-facing angle test are applied
again during the UV-space bake.
Unchanged brush/lasso strokes skip GPU updates. Each mask or feather change
uploads once; projection shifts, angle changes, and side filters reuse the mask.
Switching tabs restores their masks, and failed uploads remain pending for retry.

Codex initialization, generation requests, cancellation, and reconnection run on
a background command queue. Closing a projection discards its subsequent events;
its temporary directory is cleaned only after its in-flight operations settle.
Projection baking validates frozen triangle IDs as well as depth, so other mesh
surfaces and inference references correctly block projection into occluded UVs.

## Tests

The test target covers OBJ validation and UV-overlap diagnostics, FBX/GLB fixture
imports, hierarchy and mirrored transforms, material groups, UV channels and
texture transforms, external/embedded images, invalid resources, and import
rollback. Image tests cover PNG RGBA round trips and WIC use across COM lifetimes.
Tests also cover manual mask operations and prompt construction, an in-process WARP render/bake golden path,
and a test-only JSONL App Server executable for signed-out, missing-skill,
generation, concurrent per-tab job routing, and interrupt behavior. A real ImageGen E2E run
still requires a locally authenticated ChatGPT Codex installation.

Projection state and lifecycle live in `ProjectionWorkspace`, separately from
the UI. Tests cover tab isolation and recovery retention, transactional texture
history, real ImGui shortcut routing, and asynchronous request/cleanup races.
The Windows workflow builds the Release GUI and runs the full suite on pushes
and pull requests, with cached vcpkg binaries and archived test diagnostics.

## Model support and limits

The FBX and GLB paths support static Base Color editing, not every feature in
either format. One material's Base Color image is editable at a time; the other
material surfaces and independent inference references provide visible context.
References cannot be selected, hidden by face tools, projected into, baked, or
saved by CodexTex. The application does not unwrap or repack UVs, edit geometry,
manage PBR auxiliary maps, or write OBJ, MTL, FBX, GLB, or project files. OBJ keeps
its existing separate-PNG workflow and does not use MTL texture bindings.

| Area | Current behavior |
| --- | --- |
| Geometry | FBX polygons and GLB triangles, strips, and fans become static triangles grouped by material. Node hierarchy, geometry transforms, nonuniform scales, and mirrored winding are applied; normals use the inverse transpose. Missing normals are generated. |
| Coordinates | FBX and GLB use right-handed Y-up meters after import. OBJ remains unitless in its authored coordinates. Primary and reference models use the same format-specific conversion, with no automatic alignment or relative scale correction. |
| Images | Native Base Color images can be embedded or external PNG/JPEG. External references resolve relative to the model. A missing or undecodable authored image displays white and requires a replacement PNG before editing. An untextured material starts with a white working image. |
| UVs | The Base Color binding selects its UV set. Texture transforms are applied consistently to display and bake. OBJ/FBX bottom-left UVs are converted once; GLB top-left UVs are preserved. UVs are never automatically repaired or packed. |
| Materials | Base Color factors, supported wrap modes, alpha cutouts, and sidedness are kept separate from image pixels. GLB `MASK` and its cutoff are used by color, depth, face-ID, capture, and bake paths. FBX binary image alpha is treated as a cutout; fractional alpha is translucent. |
| Editing restrictions | Translucent/`BLEND` surfaces, missing UVs, UVs outside a single 0..1 atlas, vertex colors, and zero Base Color RGB factors are display-only. FBX layered/procedural Base Color and separate opacity textures also require conversion before editing. These limitations are shown rather than silently baked incorrectly. |
| Deformation | Skins, morph targets, and FBX geometry caches are rejected with an export-static-mesh message. Transform animation is not played; authored static transforms are imported with a warning. |
| GLB extensions | Required extensions are accepted only for `KHR_texture_transform`, `KHR_mesh_quantization`, and `KHR_materials_unlit`. Unsupported required extensions report their names. Draco, meshopt compression, and GPU instancing require export without those features. |
| Saving | Saving writes the current texture as PNG. An external PNG can be saved back to its path; an embedded image or JPEG needs a PNG destination. FBX/GLB files and their embedded images remain unchanged. |

Imports allow at most 2,000,000 triangles after expanding instances, a 512 MiB
model file, and bounded external resources/parser allocations. GLB buffer data
has a 512 MiB aggregate budget. Decoded material images have a 512 MiB aggregate
budget; each image is limited to 128 MiB encoded data, 256 MiB RGBA / 64 megapixels,
and 16,384 pixels per dimension. Metadata is checked before pixel decoding, and
remaining material-image budget is checked before allocating further images.
These are resource limits, not a guarantee that every model within them will load.

The source distributions are pinned in the repository: [ufbx v0.23.0](third_party/ufbx/README.md)
for FBX and [cgltf](third_party/cgltf/README.md) for GLB. Their upstream commit IDs,
source provenance, and MIT license texts are included in `third_party/`.
GUI builds copy the license texts into the output directory's `licenses/` folder.
