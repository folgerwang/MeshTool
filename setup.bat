@echo off
rem ===========================================================================
rem  MeshTool - dependency setup
rem
rem  Downloads every third-party dependency into .\thirdparty (gitignored).
rem  Safe to re-run: anything already present is skipped.
rem  To force a fresh download of one library, delete its thirdparty\ folder.
rem
rem  Needs: git and an internet connection.
rem ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "TP=%~dp0thirdparty"
if not exist "%TP%" mkdir "%TP%"

set "BOOST_TAG=boost-1.84.0"

where git  >nul 2>nul || (echo [ERROR] git was not found in PATH. & goto :fail)

echo.
echo === Source libraries ===
rem          folder        repository                                        tag / branch   file that proves it is there
call :clone glfw          https://github.com/glfw/glfw.git                  3.4            CMakeLists.txt                                || goto :fail
call :clone imgui         https://github.com/ocornut/imgui.git              v1.90.9        imgui.cpp                                     || goto :fail
call :clone nfd           https://github.com/mlabbe/nativefiledialog.git    release_116    src\nfd_common.c                              || goto :fail
call :clone cpp-httplib   https://github.com/yhirose/cpp-httplib.git        v0.15.3        httplib.h                                     || goto :fail
call :clone zlib          https://github.com/madler/zlib.git                v1.3.1         zlib.h                                        || goto :fail
call :clone libexpat      https://github.com/libexpat/libexpat.git          R_2_6_2        expat\lib\xmlparse.c                          || goto :fail
call :clone uriparser     https://github.com/uriparser/uriparser.git        uriparser-0.7.5 include\uriparser\Uri.h                      || goto :fail
call :clone geographiclib https://github.com/geographiclib/geographiclib.git v2.3          include\GeographicLib\UTMUPS.hpp              || goto :fail
call :clone libkml        https://github.com/libkml/libkml.git              master         src\kml\dom.h                                 || goto :fail
call :clone glm           https://github.com/g-truc/glm.git                 1.0.1          glm\glm.hpp                                   || goto :fail
call :clone stb           https://github.com/nothings/stb.git               master         stb_image_write.h                             || goto :fail
call :boost || goto :fail

echo.
echo ===========================================================================
echo  All dependencies are in place. Next:
echo.
echo    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
echo    cmake --build build --config Release
echo    run.bat
echo ===========================================================================
exit /b 0

:fail
echo.
echo [FAILED] Setup did not complete - see the message above. Re-run setup.bat after fixing it.
exit /b 1


rem ---------------------------------------------------------------------------
rem  :clone <folder> <repo-url> <tag> <check-file>
rem ---------------------------------------------------------------------------
:clone
if exist "%TP%\%~1\%~4" (
    echo   [ok ] %~1
    exit /b 0
)
echo   [get] %~1 @ %~3
if exist "%TP%\%~1" rmdir /S /Q "%TP%\%~1"
git -c advice.detachedHead=false clone --quiet --depth 1 --branch %~3 %~2 "%TP%\%~1"
if errorlevel 1 (
    echo [ERROR] git clone of %~2 failed.
    exit /b 1
)
exit /b 0


rem ---------------------------------------------------------------------------
rem  Boost: libkml only needs scoped_ptr / intrusive_ptr, so instead of the
rem  ~125 MB Boost archive we fetch just the header-only modules involved and
rem  merge them into thirdparty\boost\boost.
rem ---------------------------------------------------------------------------
:boost
if exist "%TP%\boost\boost\scoped_ptr.hpp" (
    echo   [ok ] boost headers
    exit /b 0
)
echo   [get] boost headers subset @ %BOOST_TAG%
if exist "%TP%\_boost_src" rmdir /S /Q "%TP%\_boost_src"
for %%M in (smart_ptr config assert core throw_exception static_assert type_traits move predef) do (
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch %BOOST_TAG% https://github.com/boostorg/%%M.git "%TP%\_boost_src\%%M"
    if errorlevel 1 (
        echo [ERROR] git clone of boostorg/%%M failed.
        exit /b 1
    )
    xcopy /E /I /Y /Q "%TP%\_boost_src\%%M\include\boost" "%TP%\boost\boost" >nul
)
rmdir /S /Q "%TP%\_boost_src"
exit /b 0

