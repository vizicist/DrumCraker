@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
pushd "%ROOT%" >nul
if errorlevel 1 (
    echo ERROR: Could not enter project directory.
    exit /b 1
)

echo === DrumCraker Windows VST3 Build ===
echo Project: %CD%

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
    if not defined VSINSTALL (
        for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%I"
    )
)

if not defined VSINSTALL (
    for %%I in (
        "%ProgramFiles%\Microsoft Visual Studio\18\Community"
        "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
        "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
    ) do (
        if exist "%%~I\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=%%~I"
    )
)

if not defined VSINSTALL (
    echo ERROR: Visual Studio with C++ build tools was not found.
    echo Install Visual Studio 2022 or newer with the Desktop development with C++ workload.
    popd >nul
    exit /b 1
)

set "VSDEV=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEV%" set "VSDEV=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" (
    echo ERROR: Could not find VsDevCmd.bat or vcvars64.bat under:
    echo   %VSINSTALL%
    popd >nul
    exit /b 1
)

set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE (
    set "CMAKE_EXE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)
if not exist "%CMAKE_EXE%" (
    echo ERROR: CMake was not found on PATH or inside Visual Studio.
    popd >nul
    exit /b 1
)

set "NINJA_EXE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%NINJA_EXE%" (
    set "NINJA_EXE="
    for /f "delims=" %%I in ('where ninja 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%I"
)

echo Visual Studio: %VSINSTALL%
echo CMake: %CMAKE_EXE%
if defined NINJA_EXE echo Ninja: %NINJA_EXE%

echo.
echo Initializing MSVC x64 build environment...
call "%VSDEV%" -arch=x64 -host_arch=x64
if errorlevel 1 (
    echo ERROR: Failed to initialize the Visual Studio build environment.
    popd >nul
    exit /b 1
)

echo.
echo Cleaning previous build artifacts...
if exist "%CD%\build" rmdir /s /q "%CD%\build"
if exist "%CD%\releases" rmdir /s /q "%CD%\releases"

echo.
echo Configuring Release build...
if defined NINJA_EXE (
    "%CMAKE_EXE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%"
) else (
    "%CMAKE_EXE%" -S . -B build -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    popd >nul
    exit /b 1
)

echo.
echo Building VST3 target...
"%CMAKE_EXE%" --build build --config Release --target DrumCrakerVST_VST3 --parallel
if errorlevel 1 (
    echo ERROR: Build failed.
    popd >nul
    exit /b 1
)

set "VST3_DIR=%CD%\releases\DrumCraker.vst3"
set "VST3_DLL=%VST3_DIR%\Contents\x86_64-win\DrumCraker.vst3"

if not exist "%VST3_DLL%" (
    echo ERROR: Build completed, but the VST3 binary was not found:
    echo   %VST3_DLL%
    popd >nul
    exit /b 1
)

echo.
echo === Build completed ===
echo VST3 bundle:
echo   %VST3_DIR%

popd >nul
exit /b 0
