@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
pushd "%ROOT%" >nul
if errorlevel 1 (
    echo ERROR: Could not enter project directory.
    exit /b 1
)

set "VST3_ROOT=%ProgramFiles%\Common Files\VST3"
set "SOURCE_BUNDLE=%CD%\releases\DrumCraker.vst3"
set "SOURCE_KIT=%CD%\tchackpoum-drumgizmo-kit"
set "TARGET_BUNDLE=%VST3_ROOT%\DrumCraker.vst3"
set "TARGET_KIT=%VST3_ROOT%\tchackpoum-drumgizmo-kit"
set "TARGET_DLL=%TARGET_BUNDLE%\Contents\x86_64-win\DrumCraker.vst3"

echo Installing DrumCraker to system VST3...

if not exist "%SOURCE_BUNDLE%\" (
    echo ERROR: Missing releases\DrumCraker.vst3. Run build_windows.cmd first.
    popd >nul
    exit /b 1
)

if not exist "%SOURCE_BUNDLE%\Contents\x86_64-win\DrumCraker.vst3" (
    echo ERROR: Missing built VST3 binary. Run build_windows.cmd first.
    popd >nul
    exit /b 1
)

if not exist "%SOURCE_KIT%\" (
    echo ERROR: Missing tchackpoum-drumgizmo-kit directory.
    popd >nul
    exit /b 1
)

call :ensure_admin
if errorlevel 1 (
    popd >nul
    exit /b 1
)

if not exist "%VST3_ROOT%\" (
    mkdir "%VST3_ROOT%"
    if errorlevel 1 (
        echo ERROR: Could not create system VST3 directory.
        popd >nul
        exit /b 1
    )
)

echo Copying VST3 bundle...
call :mirror_dir "%SOURCE_BUNDLE%" "%TARGET_BUNDLE%"
if errorlevel 1 (
    popd >nul
    exit /b 1
)

echo Copying drum kit...
call :mirror_dir "%SOURCE_KIT%" "%TARGET_KIT%"
if errorlevel 1 (
    popd >nul
    exit /b 1
)

if not exist "%TARGET_DLL%" (
    echo ERROR: Installed VST3 binary is missing.
    popd >nul
    exit /b 1
)

if not exist "%TARGET_KIT%\" (
    echo ERROR: Installed drum kit directory is missing.
    popd >nul
    exit /b 1
)

echo.
echo Install complete.

popd >nul
exit /b 0

:ensure_admin
fltmc >nul 2>&1
if not errorlevel 1 (
    exit /b 0
)

echo Administrator rights are required to install into:
echo   %VST3_ROOT%
echo Requesting elevation...

powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = Start-Process -FilePath '%~f0' -Verb RunAs -Wait -PassThru; exit $p.ExitCode"
if errorlevel 1 (
    echo ERROR: Elevation request failed.
    exit /b 1
)

exit /b 0

:mirror_dir
set "COPY_SOURCE=%~1"
set "COPY_TARGET=%~2"

robocopy "%COPY_SOURCE%" "%COPY_TARGET%" /MIR /R:2 /W:2 /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
    echo ERROR: Copy failed.
    exit /b 1
)

exit /b 0
