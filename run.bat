@echo off
rem ===========================================================================
rem  MeshTool launcher
rem
rem  Makes sure the AI segmentation model is available (Ollama + qwen3.8),
rem  then starts MeshTool. Arguments are passed on (e.g. run.bat scene.mtscene).
rem  Without Ollama/the model MeshTool still runs; Tools > Segment Scene then
rem  uses colour and height only.
rem ===========================================================================
setlocal EnableExtensions
rem Compiled shaders are copied next to MeshTool.exe at build time.
rem Start from the repo root: land_ocean_ice_8192.png and mesh_tool.cfg are read from here.
cd /d "%~dp0"

set "QWEN_MODEL=qwen3.8:latest"

if not exist "build\Release\MeshTool.exe" (
    echo build\Release\MeshTool.exe not found - run setup.bat, then build with CMake first.
    pause
    exit /b 1
)

call :ensure_ai
echo.
build\Release\MeshTool.exe %*
pause
exit /b 0


rem ---------------------------------------------------------------------------
rem  Ollama installed, server running, model pulled. Never fatal.
rem ---------------------------------------------------------------------------
:ensure_ai
echo === AI segmentation model (%QWEN_MODEL%) ===

rem 1. Ollama installed?
set "OLLAMA=ollama"
where ollama >nul 2>nul && goto :have_ollama
if exist "%LOCALAPPDATA%\Programs\Ollama\ollama.exe" (
    set "OLLAMA=%LOCALAPPDATA%\Programs\Ollama\ollama.exe"
    goto :have_ollama
)
echo   Ollama is not installed. It runs the vision model used by Tools ^> Segment Scene.
where winget >nul 2>nul || (
    echo   Install it from https://ollama.com/download and run this again.
    echo   Continuing without the model.
    exit /b 0
)
choice /c YN /m "  Install Ollama now with winget"
if errorlevel 2 (
    echo   Continuing without the model.
    exit /b 0
)
winget install --id Ollama.Ollama -e --accept-source-agreements --accept-package-agreements
if exist "%LOCALAPPDATA%\Programs\Ollama\ollama.exe" (
    set "OLLAMA=%LOCALAPPDATA%\Programs\Ollama\ollama.exe"
) else (
    where ollama >nul 2>nul || (
        echo   [!] Ollama install did not complete. Continuing without the model.
        exit /b 0
    )
)

:have_ollama
rem 2. Server running? ("ollama list" fails when it is not.)
"%OLLAMA%" list >nul 2>nul && goto :server_up
echo   Starting the Ollama server...
start "Ollama" /min "%OLLAMA%" serve
for /l %%i in (1,1,30) do (
    timeout /t 1 /nobreak >nul
    "%OLLAMA%" list >nul 2>nul && goto :server_up
)
echo   [!] The Ollama server did not start. Continuing without the model.
exit /b 0

:server_up
echo   [ok ] Ollama server is running

rem 3. Model pulled?
"%OLLAMA%" show %QWEN_MODEL% >nul 2>nul && (
    echo   [ok ] %QWEN_MODEL% is installed
    exit /b 0
)
echo   %QWEN_MODEL% is not installed (about 17 GB download; needs a GPU with ~20 GB VRAM).
choice /c YN /m "  Download it now"
if errorlevel 2 (
    echo   Continuing without the model.
    exit /b 0
)
"%OLLAMA%" pull %QWEN_MODEL%
if errorlevel 1 (
    echo   [!] Download failed. Continuing without the model.
    exit /b 0
)
echo   [ok ] %QWEN_MODEL% is installed
exit /b 0
