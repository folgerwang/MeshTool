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
