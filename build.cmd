@echo off
rem Configure and build build\mcg_recomp.exe with MSVC + Ninja, x64 host (the
rem lifted machine is 32-bit, but the host is 64-bit: see CMakeLists.txt).
rem Needs Visual Studio 2022 (any edition, or the Build Tools) with the C++ tools.
rem   set BUILD_DIR=build-trace & set CMAKE_ARGS=-DCMAKE_C_FLAGS=-DRECOMP_TRACE
rem builds the bring-up trace variant instead.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do set "VS=%%i"
if not defined VS for /d %%e in ("%ProgramFiles%\Microsoft Visual Studio\2022\*") do if exist "%%e\VC\Auxiliary\Build\vcvarsall.bat" set "VS=%%e"
if not defined VS (echo Visual Studio 2022 not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" amd64 >nul || exit /b 1
if not defined BUILD_DIR set "BUILD_DIR=build"
if not defined BUILD_TYPE set "BUILD_TYPE=RelWithDebInfo"
if not exist %BUILD_DIR%\build.ninja cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %CMAKE_ARGS% || exit /b 1
cmake --build %BUILD_DIR% %*
