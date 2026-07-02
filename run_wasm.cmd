@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ============================================================================
REM  DrumCraker - run the WebAssembly demo
REM  Starts the local static server (AudioWorklet needs http, not file://) in a
REM  separate window and opens the demo page in your default browser.
REM  Usage:  run_wasm.cmd [port]        (default port 8080)
REM ============================================================================

set "ROOT=%~dp0"
set "WEB=%ROOT%web"
set "SERVER=%WEB%\serve.mjs"
set "WASM=%WEB%\public\drumcraker.wasm"

set "PORT=%~1"
if not defined PORT set "PORT=8080"

echo === DrumCraker WebAssembly Demo ===

if not exist "%SERVER%" (
    echo ERROR: Server script not found: %SERVER%
    exit /b 1
)

if not exist "%WASM%" (
    echo WARNING: %WASM% not found.
    echo Build it first with:  build_wasm.cmd
    echo.
)

REM --- Locate Node.js ----------------------------------------------------------
REM Prefer a Node on PATH; otherwise fall back to the one bundled with emsdk.
set "NODE_EXE="
for /f "delims=" %%I in ('where node 2^>nul') do if not defined NODE_EXE set "NODE_EXE=%%I"

if not defined NODE_EXE (
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
    if defined EMSDK_ROOT (
        for /f "delims=" %%I in ('dir /b /s "!EMSDK_ROOT!\node\*\bin\node.exe" 2^>nul') do (
            if not defined NODE_EXE set "NODE_EXE=%%I"
        )
    )
)

if not defined NODE_EXE (
    echo ERROR: Node.js was not found on PATH or in the Emscripten SDK.
    echo Install Node.js from https://nodejs.org/ and try again.
    exit /b 1
)

echo Node:   %NODE_EXE%
echo Server: http://localhost:%PORT%
echo.

REM --- Start the server in its own window --------------------------------------
echo Starting server ^(close its window to stop it^)...
start "DrumCraker WASM server" cmd /k ""%NODE_EXE%" "%SERVER%" %PORT%"

REM Give the server a moment to bind the port, then open the browser.
timeout /t 2 /nobreak >nul
start "" "http://localhost:%PORT%"

echo.
echo The demo should now be open in your browser. Click "Start Audio".
echo To stop the server, close the "DrumCraker WASM server" window.
exit /b 0
