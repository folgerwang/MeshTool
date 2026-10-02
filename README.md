# MeshTool

Mesh tool for Google Earth mesh capture.

Built with GLFW + Dear ImGui + Vulkan (the old Qt6/OpenGL UI was replaced).
Includes a GL hook (`opengl32.dll` proxy / injectable `meshtool_hook.dll`) for
live capture from Google Earth Pro.

## Requirements

- Windows 10+, Visual Studio 2022 (x64), CMake >= 3.16, git
- Vulkan SDK (provides `glslc` for shader compilation)

## Setup

Run `setup.bat` once. It downloads every dependency into `thirdparty/`
(gitignored) - nothing outside the repo is used. Re-running it skips anything
already present; delete a `thirdparty/<lib>` folder to re-download that one.

| Library | Version | How |
|---|---|---|
| GLFW | 3.4 | git |
| Dear ImGui | 1.90.9 | git |
| nativefiledialog | release_116 | git |
| cpp-httplib | 0.15.3 | git |
| zlib (+ minizip) | 1.3.1 | git |
| expat | 2.6.2 | git |
| uriparser | 0.7.5 | git |
| GeographicLib | 2.3 | git |
| libkml | master | git |
| Boost headers (smart_ptr and deps only) | 1.84.0 | git |
| glm | 1.0.1 | git |
| stb (stb_image, stb_image_write) | master | git |

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The source libraries are built as static libs by `cmake/ThirdParty.cmake`.
glm and stb are header-only. The compiled shaders are copied next to `MeshTool.exe`.

## Run

`run.bat` - starts `build\Release\MeshTool.exe` from the repo root, where
`land_ocean_ice_8192.png` and `mesh_tool.cfg` live.

After pulling hook changes, redeploy the proxy (`deploy_hook.bat` as
administrator, option 2): MeshTool and `opengl32.dll` share the capture buffer
layout (256 MB) and must match.

## Live capture

Launch Google Earth from MeshTool (Capture > Launch Google Earth, or Navigate &
Capture), then Capture Frame or F12 inside Google Earth.

- **GPS.** MeshTool opens a KML NetworkLink in Google Earth that reports the
  view to `http://127.0.0.1:47321/view` whenever the camera stops. Captures are
  placed in East/North/Up metres at a GPS origin (the first capture's look-at
  point); the viewport corner shows the pivot's latitude/longitude. Heights are
  GE's (above sea level). Without the link, a camera-based frame is used.
- **Merging.** A capture next to or overlapping the current one is merged into
  it (placed through tiles both contain, or else GPS); any other capture
  replaces it. Tiles already captured are dropped.
- **Levels of detail.** Google Earth draws coarse tiles under finer ones; the
  covered coarse triangles are removed.
- Diagnostics: `C:\Users\Public\meshtool_capture.log` (placement, GPS checks,
  LOD filter) and `C:\Users\Public\meshtool_proxy.log` (hook side).

## Segmentation

Tools > Segment Scene splits the scene into objects: each building, tree and
car on its own (`building_012`, `tree_007`, `car_003`), plus one object each
for road, plants, water and other ground. Needs [Ollama](https://ollama.com)
with a vision model (default `qwen3.8:latest`); without it, classes come from
colour and height only.

1. The mesh is rendered top-down (0.25 m/pixel): colour, height, coverage.
2. A ground model (lowest points flooded across max-35% slopes, local plane
   fits) gives height above ground - exact on slopes and hills.
3. Raised areas split into instances at height breaks (1.5 m, or 8% of the
   height for towers, so terraced crowns stay whole); steep facade pixels join
   the highest roof they hang from. Tiers / towers / rooftop structures that
   stand on a lower part holding most of their outline are merged into it, and
   so are pieces too slender to stand alone (facade ledges and fins: footprint
   under 3% of height squared) into the taller part they touch. Ground split
   into colour regions.
4. The model labels numbered regions on 768 px tiles, told which are raised;
   answers are schema-constrained JSON.
5. Triangles seen from above join the object under them; walls inherit
   their roof's object along the mesh, flowing only downward (so ground never
   climbs a wall); walls in tiles not stitched to their roof look behind
   themselves (against the normal) for a roof that reaches their top; stray
   triangles take the object of their mesh neighbours,
   and meshes are split per object. Objects are saved in `.mtscene` and name the
   glTF nodes.

When segmentation finishes, the 3D viewport is saved twice for checking -
class colours and original textures - as
`screenshots\check_<time>_segment.png` / `_original.png` in the MeshTool folder.
Tools > Save Check Screenshots takes the pair again from the current view.
Each run also saves its input as `last_input.mtscene` in the debug folder, so it
can be re-run and checked offline:

```
MeshTool --segment last_input.mtscene out.mtscene [--no-model] [--debug-dir d] [--dump]
MeshTool out.mtscene --shots prefix [--frame building_012] [--yaw 30] [--pitch -20] [--zoom 0.85]
```

`--dump` adds raw rasters (`top.f32`, `object.i32`), `objects.txt` and per-triangle
labels (`tris.bin`) to the debug folder; `--shots` opens a scene, aims at an
object, saves the check-screenshot pair and exits.

Scene panel > Objects: colour by class, show/hide classes. Debug images
(orthophoto, regions, classes, marked tiles) and a log go to
`C:\Users\Public\meshtool_segment\`. Headless:
`MeshTool.exe --segment in.mtscene out.mtscene [--no-model] [--res m]`.

## Scene files

File > Save Scene / Open Scene (Ctrl+S / Ctrl+O): `.mtscene`, MeshTool's
lossless native format - meshes, textures as captured, GPS origin.

## Viewport

| | Maya style | Unreal style |
|---|---|---|
| Orbit / look | Alt + LMB | RMB drag (look around) |
| Pan | Alt + MMB | MMB, or LMB + RMB |
| Dolly / move | Alt + RMB | LMB drag (forward/back + turn) |
| Fly | | RMB + W/A/S/D, Q/E down/up; wheel = speed, Shift = faster |

Wheel zooms to the pivot, F frames everything.

## Export

File > Export Mesh writes glTF 2.0, picked by the extension you type:

- `.glb` - one self-contained file, textures embedded
- `.gltf` - JSON + `<name>.bin`, textures in `<name>/textures/`
- `.ma` - Maya ASCII

Meshes keep their world-space offset as node translations (source data is
Z-up; a root node rotates it to glTF's Y-up). Materials use
`KHR_materials_unlit`, since the captured textures already have lighting baked in.

## Targets

- `MeshTool` - main app
- `GLHookDLL` (`opengl32.dll`) - proxy DLL, deploy with `deploy_hook.bat`
- `GLHookInject` (`meshtool_hook.dll`) - injectable hook used by live capture
- `GLHookTest` (`opengl32_test.dll`) - forwarding-only proxy for debugging
- `test_inject` - inject `meshtool_hook.dll` into a running process by name or PID
