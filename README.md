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
