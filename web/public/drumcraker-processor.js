// DrumCraker AudioWorklet processor.
//
// Hosts the WebAssembly engine on the audio thread. The main thread ships the
// raw .wasm bytes (fetched there, since AudioWorklets can't fetch) plus the
// decoded sample buffers and kit structure; everything below runs per audio
// block with no allocation, mirroring the native plugin's lock-free renderer.

class DrumCrakerProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.ready = false;
    this.exports = null;
    this.expectedBufferId = 0;
    this.pending = [];      // scheduled notes: {atFrame, note, vel}
    this.msgQueue = [];     // messages received before WASM finished loading

    this.port.onmessage = (e) => {
      if (!this.ready) this.msgQueue.push(e.data);
      else this.onMessage(e.data);
    };

    const wasmBytes = options.processorOptions && options.processorOptions.wasmBytes;
    if (wasmBytes) this.instantiate(wasmBytes);
  }

  async instantiate(bytes) {
    const imports = {
      env: { emscripten_notify_memory_growth: () => {} },
      wasi_snapshot_preview1: {
        fd_close: () => 0,
        fd_write: () => 0,
        fd_seek: () => 0,
      },
    };
    const { instance } = await WebAssembly.instantiate(bytes, imports);
    const ex = instance.exports;
    if (ex._initialize) ex._initialize();
    ex.dc_init(sampleRate);
    ex.dc_seed((Date.now() & 0xffffffff) >>> 0);
    this.exports = ex;
    this.ready = true;
    this.port.postMessage({ type: 'ready' });
    // Drain anything that arrived while we were instantiating.
    const queued = this.msgQueue;
    this.msgQueue = [];
    for (const m of queued) this.onMessage(m);
  }

  // Float32 view over engine memory (re-derived each time; the backing buffer
  // can change when WASM memory grows during kit loading).
  view(ptr, len) {
    return new Float32Array(this.exports.memory.buffer, ptr, len);
  }

  onMessage(msg) {
    const ex = this.exports;
    switch (msg.type) {
      case 'loadKitBegin':
        ex.dc_clear_kit();
        this.expectedBufferId = 0;
        break;

      case 'buffer': {
        const src = msg.data; // Float32Array (transferred)
        const bid = ex.dc_create_buffer(src.length);
        // bid is expected to equal msg.index (buffers created in order).
        const ptr = ex.dc_buffer_ptr(bid);
        this.view(ptr, src.length).set(src);
        this.expectedBufferId++;
        break;
      }

      case 'kitStructure': {
        for (const inst of msg.instruments) {
          const ii = ex.dc_add_instrument();
          for (const s of inst.samples) {
            const si = ex.dc_add_sample(ii, s.power);
            for (const r of s.refs) ex.dc_add_audioref(ii, si, r.buffer, r.routing);
          }
        }
        for (const m of msg.midiMap) ex.dc_set_midi(m.note, m.instr);
        ex.dc_finalize_kit();
        this.port.postMessage({ type: 'kitReady' });
        break;
      }

      case 'params':
        ex.dc_set_params(msg.db, msg.vel, msg.tim, msg.rr);
        break;

      case 'noteOn':
        // atFrame === 0 means "as soon as possible" (manual pad hits).
        this.pending.push({ atFrame: msg.atFrame || 0, note: msg.note, vel: msg.velocity });
        break;

      case 'panic':
        // Rebuild-free way to silence: clear pending and let voices ring out.
        this.pending.length = 0;
        break;
    }
  }

  // NOTE: the AudioWorkletProcessor callback is process(inputs, outputs, params).
  // Outputs is the SECOND argument.
  process(inputs, outputs) {
    try {
      return this._process(outputs);
    } catch (err) {
      if (!this._reportedError) {
        this._reportedError = true;
        this.port.postMessage({ type: 'error', message: String((err && err.stack) || err) });
      }
      return true; // keep the processor alive so the error surfaces
    }
  }

  _process(outputs) {
    const out = outputs[0];
    if (!this.ready || !out || out.length === 0) return true;

    const frames = out[0].length; // normally 128
    const ex = this.exports;

    // Fire any notes due within this block.
    if (this.pending.length) {
      const blockEnd = currentFrame + frames;
      let w = 0;
      for (let i = 0; i < this.pending.length; i++) {
        const p = this.pending[i];
        if (p.atFrame <= blockEnd) {
          ex.dc_note_on(p.note, p.vel);
        } else {
          this.pending[w++] = p;
        }
      }
      this.pending.length = w;
    }

    ex.dc_render(frames);

    // Report active voice count a few times per second for the UI.
    this._blockCount = (this._blockCount || 0) + 1;
    if (this._blockCount % 16 === 0) {
      this.port.postMessage({ type: 'voices', n: ex.dc_active_voices() });
    }

    const l = this.view(ex.dc_out_l(), frames);
    const r = this.view(ex.dc_out_r(), frames);

    out[0].set(l);
    if (out.length > 1) out[1].set(r);
    else {
      // mono output: sum
      const o = out[0];
      for (let i = 0; i < frames; i++) o[i] = 0.5 * (l[i] + r[i]);
    }
    return true;
  }
}

registerProcessor('drumcraker-processor', DrumCrakerProcessor);
