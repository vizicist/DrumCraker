#!/usr/bin/env bash
# Build the DrumCraker WebAssembly engine.
#
# Produces web/public/drumcraker.wasm -- a standalone WASM reactor module with
# no Emscripten JS glue, so it can be instantiated by hand inside an
# AudioWorklet (see public/drumcraker-processor.js).
#
# Requires the Emscripten SDK. On this machine emcc is a Python script, so we
# point EMSDK_PYTHON at a real interpreter and invoke emcc.py directly.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/public/drumcraker.wasm"

EXPORTS='_dc_init,_dc_seed,_dc_clear_kit,_dc_create_buffer,_dc_buffer_ptr,_dc_add_instrument,_dc_add_sample,_dc_add_audioref,_dc_set_midi,_dc_finalize_kit,_dc_set_params,_dc_note_on,_dc_render,_dc_out_l,_dc_out_r,_dc_active_voices'

FLAGS=(
    -O3 -std=c++17 -fno-exceptions -fno-rtti
    -sSTANDALONE_WASM=1
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=33554432
    -sEXPORTED_FUNCTIONS="$EXPORTS"
    --no-entry
    -o "$OUT"
)

echo "Building $OUT ..."

# Prefer emcc on PATH. If it fails to launch (e.g. the emcc wrapper can't find
# Python), fall back to invoking emcc.py with an explicit interpreter.
if command -v emcc >/dev/null 2>&1 && emcc --version >/dev/null 2>&1; then
    emcc "$HERE/src/drumcraker.cpp" "${FLAGS[@]}"
else
    EMCC_PY="${EMCC_PY:-$(command -v emcc.py || echo /c/Users/tjt/GitHub/emsdk/upstream/emscripten/emcc.py)}"
    : "${EMSDK_PYTHON:=/c/Users/tjt/AppData/Local/Programs/Python/Python311/python.exe}"
    export EMSDK_PYTHON
    echo "emcc launcher unavailable; using $EMSDK_PYTHON $EMCC_PY"
    "$EMSDK_PYTHON" "$EMCC_PY" "$HERE/src/drumcraker.cpp" "${FLAGS[@]}"
fi

echo "Done: $(ls -la "$OUT" | awk '{print $5}') bytes"
