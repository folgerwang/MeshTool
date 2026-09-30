@echo off
echo.
echo MeshTool GL Hook Deployer
echo =========================
echo.
echo 1 = Deploy MINIMAL test proxy (no hooks, just forwarding)
echo 2 = Deploy FULL hook proxy (with overlay + capture)
echo 3 = REMOVE proxy DLL (restore original Google Earth)
echo.
set /p choice="Choice (1/2/3): "

set TARGET="C:\Program Files\Google\Google Earth Pro\client\opengl32.dll"

if "%choice%"=="1" (
    copy /Y "G:\work\MeshTool\build\Release\opengl32_test.dll" %TARGET%
    echo Deployed MINIMAL test proxy.
) else if "%choice%"=="2" (
    copy /Y "G:\work\MeshTool\build\Release\opengl32.dll" %TARGET%
    echo Deployed FULL hook proxy.
) else if "%choice%"=="3" (
    del /F %TARGET% 2>nul
    echo Removed proxy DLL.
) else (
    echo Invalid choice.
)

echo.
echo Check C:\meshtool_hook.log after launching Google Earth.
pause
