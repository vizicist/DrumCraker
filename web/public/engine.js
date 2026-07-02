// DrumCraker web engine (main-thread controller).
//
// Responsibilities that JUCE handles in the native plugin live here:
//   * WAV decoding (16/24/32-bit PCM + float, any channel count)
//   * sample-rate conversion to the AudioContext rate
//   * DrumGizmo XML parsing (kit / instrument / midimap)
// The decoded PCM and kit structure are shipped to the AudioWorklet, which
// owns the WASM engine that actually selects and mixes samples.

const ROUTE_LEFT = 0, ROUTE_RIGHT = 1, ROUTE_BOTH = 2;

// ---------------------------------------------------------------------------
// WAV decoding
// ---------------------------------------------------------------------------

// Decode a WAV ArrayBuffer into { sampleRate, channels: [Float32Array, ...] }.
function decodeWav(arrayBuffer) {
  const dv = new DataView(arrayBuffer);
  const u8 = new Uint8Array(arrayBuffer);
  if (readTag(u8, 0) !== 'RIFF' || readTag(u8, 8) !== 'WAVE')
    throw new Error('Not a WAV file');

  let pos = 12;
  let fmt = null;
  let dataOffset = -1, dataLen = 0;

  while (pos + 8 <= dv.byteLength) {
    const id = readTag(u8, pos);
    const size = dv.getUint32(pos + 4, true);
    const body = pos + 8;
    if (id === 'fmt ') {
      let audioFormat = dv.getUint16(body, true);
      const numChannels = dv.getUint16(body + 2, true);
      const sampleRate = dv.getUint32(body + 4, true);
      const bitsPerSample = dv.getUint16(body + 14, true);
      if (audioFormat === 0xfffe && size >= 40) {
        // WAVE_FORMAT_EXTENSIBLE: real format tag is in the sub-format GUID.
        audioFormat = dv.getUint16(body + 24, true);
      }
      fmt = { audioFormat, numChannels, sampleRate, bitsPerSample };
    } else if (id === 'data') {
      dataOffset = body;
      dataLen = size;
    }
    pos = body + size + (size & 1); // chunks are word-aligned
  }

  if (!fmt || dataOffset < 0) throw new Error('Malformed WAV (missing fmt/data)');

  const { audioFormat, numChannels, sampleRate, bitsPerSample } = fmt;
  const bytesPerSample = bitsPerSample >> 3;
  const frameCount = Math.floor(dataLen / (bytesPerSample * numChannels));
  const channels = [];
  for (let c = 0; c < numChannels; c++) channels.push(new Float32Array(frameCount));

  const isFloat = audioFormat === 3;
  for (let f = 0; f < frameCount; f++) {
    for (let c = 0; c < numChannels; c++) {
      const off = dataOffset + (f * numChannels + c) * bytesPerSample;
      channels[c][f] = readSample(dv, off, bitsPerSample, isFloat);
    }
  }
  return { sampleRate, channels };
}

function readTag(u8, off) {
  return String.fromCharCode(u8[off], u8[off + 1], u8[off + 2], u8[off + 3]);
}

function readSample(dv, off, bits, isFloat) {
  if (isFloat) {
    if (bits === 32) return dv.getFloat32(off, true);
    if (bits === 64) return dv.getFloat64(off, true);
  }
  switch (bits) {
    case 8:  return (dv.getUint8(off) - 128) / 128;
    case 16: return dv.getInt16(off, true) / 32768;
    case 24: {
      const b0 = dv.getUint8(off), b1 = dv.getUint8(off + 1), b2 = dv.getUint8(off + 2);
      let v = b0 | (b1 << 8) | (b2 << 16);
      if (v & 0x800000) v |= ~0xffffff; // sign-extend
      return v / 8388608;
    }
    case 32: return dv.getInt32(off, true) / 2147483648;
    default: throw new Error('Unsupported bit depth: ' + bits);
  }
}

// Linear resampling to a target rate. Transparent enough for a demo; the
// native plugin uses Lagrange interpolation but the difference is inaudible
// for one-shot drum hits.
function resample(input, srcRate, dstRate) {
  if (srcRate === dstRate) return input;
  const ratio = srcRate / dstRate;
  const outLen = Math.max(1, Math.round(input.length / ratio));
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) {
    const srcPos = i * ratio;
    const i0 = Math.floor(srcPos);
    const i1 = Math.min(i0 + 1, input.length - 1);
    const frac = srcPos - i0;
    out[i] = input[i0] * (1 - frac) + input[i1] * frac;
  }
  return out;
}

function routeForChannelName(name) {
  const last = name.charAt(name.length - 1);
  if (last === 'L' || /left/i.test(name)) return ROUTE_LEFT;
  if (last === 'R' || /right/i.test(name)) return ROUTE_RIGHT;
  return ROUTE_BOTH;
}

// ---------------------------------------------------------------------------
// Kit descriptors
// ---------------------------------------------------------------------------
// Common shape both loaders produce, fed to materialize():
//   { name, description, instruments:[{name, samples:[{power, audio:[{path, channel, routing}]}]}],
//     midimap:[{note, instr}] }

// Parse a bundled web-kit manifest (produced by tools/build-web-kit.mjs).
// Each sample file is a stereo premix, so channel 0 -> L, channel 1 -> R.
function descriptorFromWebKit(manifest) {
  const instruments = manifest.instruments.map((inst) => ({
    name: inst.name,
    samples: inst.samples.map((s) => ({
      power: s.power,
      audio: [
        { path: s.file, channel: 0, routing: ROUTE_LEFT },
        { path: s.file, channel: 1, routing: ROUTE_RIGHT },
      ],
    })),
  }));
  return {
    name: manifest.name,
    description: manifest.description || '',
    instruments,
    midimap: manifest.midimap,
  };
}

// Parse a full DrumGizmo kit into a descriptor. `provider` resolves relative
// paths to text / ArrayBuffers; `kitPath` is the kit XML relative to root.
async function descriptorFromDrumGizmo(provider, kitPath) {
  const parser = new DOMParser();
  const kitXml = parser.parseFromString(await provider.getText(kitPath), 'text/xml');
  const drumkit = kitXml.querySelector('drumkit');
  if (!drumkit) throw new Error('Not a DrumGizmo kit (no <drumkit>)');

  const kitDir = dirname(kitPath);
  const instruments = [];

  for (const instNode of kitXml.querySelectorAll('instruments > instrument')) {
    const name = instNode.getAttribute('name');
    const file = instNode.getAttribute('file');
    const instPath = joinPath(kitDir, file);
    const instDir = dirname(instPath);

    let instDoc;
    try {
      instDoc = parser.parseFromString(await provider.getText(instPath), 'text/xml');
    } catch (e) {
      continue; // skip instruments whose files are missing
    }

    const samples = [];
    for (const sNode of instDoc.querySelectorAll('samples > sample')) {
      const power = parseFloat(sNode.getAttribute('power')) || 1;
      const audio = [];
      for (const aNode of sNode.querySelectorAll('audiofile')) {
        const channelName = aNode.getAttribute('channel') || '';
        const fileChannel = parseInt(aNode.getAttribute('filechannel') || '1', 10);
        const wavPath = joinPath(instDir, aNode.getAttribute('file'));
        audio.push({
          path: wavPath,
          channel: Math.max(0, fileChannel - 1),
          routing: routeForChannelName(channelName),
        });
      }
      if (audio.length) samples.push({ power, audio });
    }
    if (samples.length) instruments.push({ name, samples });
  }

  // MIDI map: prefer <defaultmidimap>, fall back to <kitname>_midimap.xml.
  const midimap = [];
  let midiMapPath = null;
  const dmm = kitXml.querySelector('metadata > defaultmidimap');
  if (dmm && dmm.getAttribute('src')) midiMapPath = joinPath(kitDir, dmm.getAttribute('src'));
  if (!midiMapPath) {
    const base = basename(kitPath).replace(/\.xml$/i, '');
    midiMapPath = joinPath(kitDir, base + '_midimap.xml');
  }
  try {
    const mmDoc = parser.parseFromString(await provider.getText(midiMapPath), 'text/xml');
    for (const m of mmDoc.querySelectorAll('map')) {
      midimap.push({ note: parseInt(m.getAttribute('note'), 10), instr: m.getAttribute('instr') });
    }
  } catch (e) { /* kit without a midimap: pads will map instruments sequentially */ }

  return { name: drumkit.getAttribute('name') || 'Kit', description: drumkit.getAttribute('description') || '', instruments, midimap };
}

function dirname(p) { const i = p.lastIndexOf('/'); return i < 0 ? '' : p.slice(0, i); }
function basename(p) { const i = p.lastIndexOf('/'); return i < 0 ? p : p.slice(i + 1); }
function joinPath(dir, rel) {
  if (!dir) return rel;
  const parts = (dir + '/' + rel).split('/');
  const out = [];
  for (const part of parts) {
    if (part === '' || part === '.') continue;
    if (part === '..') out.pop(); else out.push(part);
  }
  return out.join('/');
}

// ---------------------------------------------------------------------------
// File providers
// ---------------------------------------------------------------------------

function httpProvider(baseUrl) {
  const base = baseUrl.endsWith('/') ? baseUrl : baseUrl + '/';
  return {
    async getText(path) {
      const r = await fetch(base + path);
      if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + path);
      return r.text();
    },
    async getArrayBuffer(path) {
      const r = await fetch(base + path);
      if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + path);
      return r.arrayBuffer();
    },
  };
}

// Provider backed by a <input webkitdirectory> FileList.
function fileListProvider(fileList) {
  const map = new Map();
  let root = '';
  const paths = [];
  for (const f of fileList) {
    const rel = f.webkitRelativePath || f.name;
    paths.push(rel);
    map.set(rel, f);
  }
  // Strip the common top-level directory the browser prepends.
  const firstSlash = paths[0] ? paths[0].indexOf('/') : -1;
  if (firstSlash >= 0) root = paths[0].slice(0, firstSlash + 1);
  const strip = (p) => (root && p.startsWith(root) ? p.slice(root.length) : p);

  const byStripped = new Map();
  for (const [k, v] of map) byStripped.set(strip(k), v);

  return {
    root,
    listKitXml() {
      // Candidate top-level kit XMLs (have a sibling *_midimap or contain <drumkit>).
      const xmls = [];
      for (const k of byStripped.keys()) if (/\.xml$/i.test(k) && !/midimap/i.test(k)) xmls.push(k);
      return xmls;
    },
    async getText(path) {
      const f = byStripped.get(path);
      if (!f) throw new Error('Missing file: ' + path);
      return f.text();
    },
    async getArrayBuffer(path) {
      const f = byStripped.get(path);
      if (!f) throw new Error('Missing file: ' + path);
      return f.arrayBuffer();
    },
  };
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

class DrumCrakerEngine {
  constructor() {
    this.ctx = null;
    this.node = null;
    this.analyser = null;
    this.ready = false;
    this.kit = null; // last materialized descriptor (for UI)
    this.voiceCount = 0;
    this.onKitReady = null;
    this.onProgress = null;
  }

  async init() {
    if (this.ctx) return;
    this.ctx = new (window.AudioContext || window.webkitAudioContext)();
    const wasmBytes = await (await fetch('drumcraker.wasm')).arrayBuffer();
    await this.ctx.audioWorklet.addModule('drumcraker-processor.js');

    this.node = new AudioWorkletNode(this.ctx, 'drumcraker-processor', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: { wasmBytes },
    });

    this.analyser = this.ctx.createAnalyser();
    this.analyser.fftSize = 1024;
    this.node.connect(this.analyser);
    this.analyser.connect(this.ctx.destination);

    this._kitReadyResolvers = [];
    let wasmReadyResolve;
    const wasmReady = new Promise((r) => { wasmReadyResolve = r; });
    this.node.port.onmessage = (e) => {
      if (e.data.type === 'ready') {
        wasmReadyResolve();
      } else if (e.data.type === 'error') {
        this.lastError = e.data.message;
        console.error('[DrumCraker worklet]', e.data.message);
      } else if (e.data.type === 'voices') {
        this.voiceCount = e.data.n;
      } else if (e.data.type === 'kitReady') {
        this.ready = true;
        this._kitReadyResolvers.forEach((r) => r());
        this._kitReadyResolvers = [];
        if (this.onKitReady) this.onKitReady(this.kit);
      }
    };
    await wasmReady; // don't let callers load a kit before the engine exists
  }

  async resume() { if (this.ctx && this.ctx.state !== 'running') await this.ctx.resume(); }

  // --- kit loading --------------------------------------------------------

  async loadWebKit(baseUrl) {
    const provider = httpProvider(baseUrl);
    const manifest = JSON.parse(await provider.getText('kit.json'));
    const descriptor = descriptorFromWebKit(manifest);
    await this._materialize(descriptor, provider);
  }

  async loadKitFromFiles(fileList) {
    const provider = fileListProvider(fileList);
    // Find the kit XML: the one whose document has a <drumkit> root.
    const candidates = provider.listKitXml();
    let chosen = null;
    for (const c of candidates) {
      try {
        const doc = new DOMParser().parseFromString(await provider.getText(c), 'text/xml');
        if (doc.querySelector('drumkit')) { chosen = c; break; }
      } catch (e) { /* keep looking */ }
    }
    if (!chosen) throw new Error('No DrumGizmo kit XML (<drumkit>) found in folder');
    const descriptor = await descriptorFromDrumGizmo(provider, chosen);
    await this._materialize(descriptor, provider);
  }

  // Decode every referenced channel, resample, and ship to the worklet.
  async _materialize(descriptor, provider) {
    const targetRate = this.ctx.sampleRate;
    const decodedCache = new Map();   // path -> {sampleRate, channels}
    const bufferKeyToIndex = new Map(); // `${path}#${channel}` -> buffer index
    const buffers = [];               // Float32Array per index

    const getDecoded = async (path) => {
      if (decodedCache.has(path)) return decodedCache.get(path);
      const ab = await provider.getArrayBuffer(path);
      const decoded = decodeWav(ab);
      decodedCache.set(path, decoded);
      return decoded;
    };

    const bufferIndexFor = async (path, channel) => {
      const key = path + '#' + channel;
      if (bufferKeyToIndex.has(key)) return bufferKeyToIndex.get(key);
      const decoded = await getDecoded(path);
      const ch = decoded.channels[Math.min(channel, decoded.channels.length - 1)] || new Float32Array(0);
      const resampled = resample(ch, decoded.sampleRate, targetRate);
      const idx = buffers.length;
      buffers.push(resampled);
      bufferKeyToIndex.set(key, idx);
      return idx;
    };

    // Build instrument structure while collecting buffers.
    const instrNameToIndex = new Map();
    const instruments = [];
    let done = 0;
    const totalSamples = descriptor.instruments.reduce((n, i) => n + i.samples.length, 0);

    for (const inst of descriptor.instruments) {
      instrNameToIndex.set(inst.name, instruments.length);
      const outSamples = [];
      for (const s of inst.samples) {
        const refs = [];
        for (const a of s.audio) {
          const buffer = await bufferIndexFor(a.path, a.channel);
          refs.push({ buffer, routing: a.routing });
        }
        outSamples.push({ power: s.power, refs });
        done++;
        if (this.onProgress) this.onProgress(done, totalSamples);
      }
      instruments.push({ name: inst.name, samples: outSamples });
    }

    // MIDI map -> instrument indices. If a kit has no map, lay instruments out
    // chromatically starting at MIDI 36 so every pad is still playable.
    let midiMap = [];
    if (descriptor.midimap && descriptor.midimap.length) {
      for (const m of descriptor.midimap) {
        const instr = instrNameToIndex.get(m.instr);
        if (instr !== undefined && m.note >= 0 && m.note < 128) midiMap.push({ note: m.note, instr });
      }
    } else {
      instruments.forEach((_, i) => { if (36 + i < 128) midiMap.push({ note: 36 + i, instr: i }); });
    }

    // Ship to the worklet.
    this.ready = false;
    const readyPromise = new Promise((res) => this._kitReadyResolvers.push(res));
    this.node.port.postMessage({ type: 'loadKitBegin', sampleRate: targetRate });
    for (let i = 0; i < buffers.length; i++) {
      const copy = buffers[i];
      this.node.port.postMessage({ type: 'buffer', index: i, data: copy }, [copy.buffer]);
    }
    this.node.port.postMessage({ type: 'kitStructure', instruments, midiMap });

    // Descriptor for the UI: instrument names + the notes that trigger them.
    const noteForInstr = new Map();
    for (const m of midiMap) if (!noteForInstr.has(m.instr)) noteForInstr.set(m.instr, m.note);
    this.kit = {
      name: descriptor.name,
      description: descriptor.description,
      instruments: instruments.map((inst, i) => ({
        name: inst.name,
        note: noteForInstr.has(i) ? noteForInstr.get(i) : null,
        layers: inst.samples.length,
      })),
      midiMap,
      bufferCount: buffers.length,
    };

    await readyPromise;
    return this.kit;
  }

  // --- playback -----------------------------------------------------------

  setParams({ db, vel, tim, rr }) {
    if (this.node) this.node.port.postMessage({ type: 'params', db, vel, tim, rr });
  }

  // Immediate hit (pad / keyboard).
  noteOn(note, velocity = 0.85) {
    if (this.node) this.node.port.postMessage({ type: 'noteOn', note, velocity, atFrame: 0 });
  }

  // Scheduled hit at AudioContext time `when` (sequencer).
  noteAt(note, velocity, when) {
    const atFrame = Math.round(when * this.ctx.sampleRate);
    this.node.port.postMessage({ type: 'noteOn', note, velocity, atFrame });
  }

  now() { return this.ctx ? this.ctx.currentTime : 0; }

  getLevel() {
    if (!this.analyser) return 0;
    const buf = new Float32Array(this.analyser.fftSize);
    this.analyser.getFloatTimeDomainData(buf);
    let peak = 0;
    for (let i = 0; i < buf.length; i++) peak = Math.max(peak, Math.abs(buf[i]));
    return peak;
  }
}

window.DrumCrakerEngine = DrumCrakerEngine;
