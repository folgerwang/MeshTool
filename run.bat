@echo off
rem Compiled shaders are copied next to MeshTool.exe at build time.
rem Start from the repo root: land_ocean_ice_8192.png and mesh_tool.cfg are read from here.
cd /d "%~dp0"
if not exist "build\Release\MeshTool.exe" (
    echo build\Release\MeshTool.exe not found - run setup.bat, then build with CMake first.
    pause
    exit /b 1
)
build\Release\MeshTool.exe
pause
