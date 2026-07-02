@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ============================================================================
REM  DrumCraker - WebAssembly build
REM  Compiles web\src\drumcraker.cpp to web\public\drumcraker.wasm using the
REM  Emscripten SDK. Pass "kit" as an argument to also (re)generate the bundled
REM  web kit from the DrumGizmo source kit:  build_wasm.cmd kit
REM ============================================================================

set "ROOT=%~dp0"
set "WEB=%ROOT%web"
set "SRC=%WEB%\src\drumcraker.cpp"
set "OUT=%WEB%\public\drumcraker.wasm"

echo === DrumCraker WebAssembly Build ===
echo Project: %ROOT%

if not exist "%SRC%" (
    echo ERROR: Engine source not found: %SRC%
    exit /b 1
)

REM --- Locate the Emscripten SDK ------------------------------------------------
REM Honour an existing EMSDK env var first, then probe common locations.
set "EMSDK_ROOT="
if defined EMSDK if exist "%EMSDK%\emsdk_env.bat" set "EMSDK_ROOT=%EMSDK%"

if not defined EMSDK_ROOT (
    for %%D in (
        "%ROOT%..\emsdk"
        "%USERPROFILE%\emsdk"
        "%USERPROFILE%\GitHub\emsdk"
        "%USERPROFILE%\source\emsdk"
        "%USERPROFILE%\Documents\GitHub\emsdk"
        "C:\emsdk"
    ) do (
        if not defined EMSDK_ROOT if exist "%%~fD\emsdk_env.bat" set "EMSDK_ROOT=%%~fD"
    )
)

if not defined EMSDK_ROOT (
    echo ERROR: Could not find the Emscripten SDK ^(emsdk_env.bat^).
    echo Install it from https://emscripten.org/docs/getting_started/downloads.html
    echo then either add it to PATH, set the EMSDK environment variable, or place
    echo it next to this project as ..\emsdk
    exit /b 1
)

echo Emscripten SDK: %EMSDK_ROOT%
echo Activating Emscripten environment...
set "EMSDK_QUIET=1"
call "%EMSDK_ROOT%\emsdk_env.bat" >nul 2>&1

where emcc >nul 2>&1
if errorlevel 1 (
    echo ERROR: emcc is not available after activating the SDK.
    echo Try running "%EMSDK_ROOT%\emsdk install latest" and "emsdk activate latest".
    exit /b 1
)

REM --- Compile -----------------------------------------------------------------
set "EXPORTS=_dc_init,_dc_seed,_dc_clear_kit,_dc_create_buffer,_dc_buffer_ptr,_dc_add_instrument,_dc_add_sample,_dc_add_audioref,_dc_set_midi,_dc_finalize_kit,_dc_set_params,_dc_note_on,_dc_render,_dc_out_l,_dc_out_r,_dc_active_voices"

echo.
echo Cleaning previous output...
if exist "%OUT%" del /f /q "%OUT%"

echo Compiling %SRC% ...
call emcc "%SRC%" ^
    -O3 -std=c++17 -fno-exceptions -fno-rtti ^
    -sSTANDALONE_WASM=1 ^
    -sALLOW_MEMORY_GROWTH=1 ^
    -sINITIAL_MEMORY=33554432 ^
    "-sEXPORTED_FUNCTIONS=%EXPORTS%" ^
    --no-entry ^
    -o "%OUT%"

if errorlevel 1 (
    echo ERROR: emcc build failed.
    exit /b 1
)
if not exist "%OUT%" (
    echo ERROR: Build reported success but %OUT% is missing.
    exit /b 1
)

for %%A in ("%OUT%") do set "WASMSIZE=%%~zA"
echo.
echo Built: %OUT%  ^(%WASMSIZE% bytes^)

REM --- Optional: (re)generate the bundled web kit ------------------------------
set "KITJSON=%WEB%\public\kits\tchackpoum\kit.json"
set "KITSRC=%ROOT%tchackpoum-drumgizmo-kit\main_kit.xml"
set "DOKIT="
if /i "%~1"=="kit" set "DOKIT=1"
if not exist "%KITJSON%" if exist "%KITSRC%" set "DOKIT=1"

if defined DOKIT (
    if not exist "%KITSRC%" (
        echo.
        echo NOTE: DrumGizmo source kit not found at %KITSRC%
        echo Skipping web kit generation ^(the bundled kit, if present, is kept^).
    ) else (
        echo.
        echo Generating web kit from %KITSRC% ...
        call node "%WEB%\tools\build-web-kit.mjs" "%ROOT%tchackpoum-drumgizmo-kit" main_kit.xml "%WEB%\public\kits\tchackpoum" --layers 4 --rr 1 --maxdur 1.4 --name "Tchackpoum"
        if errorlevel 1 (
            echo ERROR: web kit generation failed.
            exit /b 1
        )
    )
) else (
    echo.
    echo Bundled web kit present ^(pass "kit" to force regeneration^).
)

echo.
echo === WASM build complete ===
echo Run the demo with:  run_wasm.cmd
exit /b 0
