# DrumCraker Web (WebAssembly)

A WebAssembly port of the [DrumCraker](../README.md) drum sampler plus a browser
demo that plays drum kits, tweaks the engine in real time, and loads different
DrumGizmo kits.

The DrumCraker audio core — velocity-layer selection, anti-machine-gun round
robin, and Gaussian velocity/timing humanization — is compiled to WASM and runs
on the audio thread inside an `AudioWorklet`, mirroring the lock-free design of
the native VST3/LV2 plugin. The parts JUCE normally provides (WAV decoding,
sample-rate conversion, XML parsing) are done in JavaScript before samples reach
the engine.

![architecture](#) <!-- see "How it works" below -->

## Quick start

**Windows** — from the project root, just double-click / run:

```bat
run_wasm.cmd                 :: starts the server and opens the demo in your browser
```

**Any platform:**

```bash
cd web
node serve.mjs 8080          # AudioWorklet needs http(s), not file://
# open http://localhost:8080  and click "Start Audio"
```

The bundled **Tchackpoum** kit is already generated under
`public/kits/tchackpoum/`, so no build step is required just to run the demo.

## Demo features

- **42 playable pads** — click (vertical position sets velocity) or use your
  keyboard. Each pad shows the MIDI note and how many velocity layers it has.
- **Live engine controls** — Master Volume (−60…+12 dB), Velocity Humanization,
  Timing Humanization, and Round Robin Mix, wired to the exact same parameter
  ranges and defaults as the plugin.
- **Step sequencer** — 16 steps, adjustable tempo, a few preset grooves. Rows
  (kick / snare / hi-hats / tom / ride / crash) are auto-detected from whatever
  kit is loaded. Space bar toggles play.
- **Load your own kit** — "Load kit from folder…" reads any local DrumGizmo kit
  directory (kit XML + instrument XMLs + WAVs) straight in the browser.
- **Output meter + live voice count** reported from the WASM voice manager.

## Rebuilding the WASM engine

Requires the [Emscripten SDK](https://emscripten.org/).

```bat
build_wasm.cmd      :: Windows: auto-detects emsdk, -> web\public\drumcraker.wasm
```

```bash
cd web
./build.sh          # macOS/Linux (emcc on PATH) -> public/drumcraker.wasm
```

`build.sh` produces a standalone WASM reactor module (no Emscripten JS glue) so
it can be instantiated by hand inside the AudioWorklet. If the `emcc` launcher
can't find Python it falls back to running `emcc.py` with an explicit
interpreter (`EMSDK_PYTHON`).

## Generating a web kit bundle

Full DrumGizmo kits are large (the source Tchackpoum kit is ~1.7 GB of 6-channel
24-bit WAVs). `tools/build-web-kit.mjs` converts a kit into a compact web bundle:
it keeps a spread of velocity layers, premixes each sample's multichannel WAV
down to stereo using DrumCraker's own L/R routing + RMS gain rules, trims
trailing silence, normalizes the whole kit to a shared peak, and writes 16-bit
stereo WAVs plus a `kit.json` manifest.

```bash
cd web/tools
node build-web-kit.mjs <kitDir> <kitXml> <outDir> [--layers N] [--rr N] [--maxdur S] [--name "..."]

# the bundled kit was generated with:
node build-web-kit.mjs ../../tchackpoum-drumgizmo-kit main_kit.xml ../public/kits/tchackpoum \
     --layers 4 --rr 1 --maxdur 1.4 --name "Tchackpoum"
```

To expose a new bundled kit in the demo, add an `<option value="kits/yourkit">`
to the kit `<select>` in `public/index.html`.

## How it works

```
 main thread                              audio thread (AudioWorklet)
 ───────────                              ───────────────────────────
 engine.js                                drumcraker-processor.js
   decode WAV (16/24/32-bit, N-ch)          instantiate drumcraker.wasm
   resample -> ctx sampleRate               receive PCM -> WASM heap
   parse DrumGizmo XML / kit.json           build kit (instr/samples/refs/midimap)
   ── postMessage(PCM + structure) ──▶      per 128-frame block:
   noteOn / scheduled steps ─────────▶        fire due notes  -> dc_note_on
                                              dc_render        -> mix voices
                                              copy stereo out  -> speakers
```

`drumcraker.cpp` is a JUCE-free port of `src/SampleEngine.cpp`,
`src/VoiceManager.cpp`, and the humanization in `src/PluginProcessor.cpp`. The
sample-selection maths (velocity normalization, 25% tolerance candidate pool,
weighted round-robin with last-sample penalty) and the voice model (128 voices,
smart stealing, per-channel L/R routing with `1/√n` gain compensation) match the
native implementation.

## Files

| File | Purpose |
|------|---------|
| `src/drumcraker.cpp` | WASM engine (sample selection, humanization, voices, mixing) |
| `src/lockfree_random.h` | xorshift32 RNG, verbatim from the plugin |
| `build.sh` | compile the engine to `public/drumcraker.wasm` |
| `public/drumcraker-processor.js` | AudioWorklet that hosts the WASM engine |
| `public/engine.js` | main-thread controller: decode, parse, resample, upload |
| `public/app.js` / `index.html` / `style.css` | the demo UI |
| `tools/build-web-kit.mjs` | DrumGizmo kit → compact web bundle |
| `serve.mjs` | tiny static server with correct `.wasm` MIME type |

## Notes & limitations

- Linear resampling is used in the browser (the native plugin uses Lagrange
  4-point); the difference is inaudible for one-shot drum hits.
- The web demo renders only the summed stereo **Main** mix. The native plugin's
  additional per-bus stem outputs aren't exposed in the browser.
- Loading a full multi-GB kit from a folder will decode every referenced channel
  into memory — fine for small/medium kits, heavy for very large ones.
