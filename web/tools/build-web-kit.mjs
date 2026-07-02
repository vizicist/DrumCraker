// build-web-kit.mjs
// -----------------
// Convert a full DrumGizmo kit into a compact web bundle for the DrumCraker
// web demo. For each instrument it keeps a spread of velocity layers (and a
// couple of round-robin variants per layer), premixes every sample's
// multichannel WAV down to stereo using DrumCraker's own L/R routing + RMS
// gain rules, trims trailing silence, normalizes the whole kit to a shared
// peak, and writes 16-bit stereo WAVs plus a kit.json manifest.
//
// Usage:
//   node build-web-kit.mjs <kitDir> <kitXml> <outDir> [--layers N] [--rr N] [--name "Kit Name"]
//
// Example:
//   node build-web-kit.mjs ../../tchackpoum-drumgizmo-kit main_kit.xml ../public/kits/tchackpoum \
//        --layers 6 --rr 2 --name "Tchackpoum"

import fs from 'node:fs';
import path from 'node:path';

const ROUTE_LEFT = 0, ROUTE_RIGHT = 1, ROUTE_BOTH = 2;

// ---- args ------------------------------------------------------------------
const args = process.argv.slice(2);
if (args.length < 3) {
  console.error('Usage: node build-web-kit.mjs <kitDir> <kitXml> <outDir> [--layers N] [--rr N] [--name "..."]');
  process.exit(1);
}
const kitDir = args[0];
const kitXml = args[1];
const outDir = args[2];
const opt = (flag, def) => { const i = args.indexOf(flag); return i >= 0 ? args[i + 1] : def; };
const MAX_LAYERS = parseInt(opt('--layers', '6'), 10);
const RR_PER_LAYER = parseInt(opt('--rr', '2'), 10);
const KIT_NAME = opt('--name', null);
const MAX_DUR = parseFloat(opt('--maxdur', '0')); // seconds; 0 = no cap

// ---- tiny XML helpers (kit files are simple + well-formed) -----------------
const attr = (s, name) => { const m = new RegExp(name + '\\s*=\\s*"([^"]*)"').exec(s); return m ? m[1] : null; };
const tags = (xml, tag) => {
  const re = new RegExp('<' + tag + '\\b([^>]*?)(/?)>', 'g');
  const out = []; let m;
  while ((m = re.exec(xml))) out.push(m[1]);
  return out;
};
const blocks = (xml, tag) => {
  const re = new RegExp('<' + tag + '\\b([^>]*)>([\\s\\S]*?)</' + tag + '>', 'g');
  const out = []; let m;
  while ((m = re.exec(xml))) out.push({ attrs: m[1], body: m[2] });
  return out;
};

// ---- WAV decode ------------------------------------------------------------
function decodeWav(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const tag = (o) => String.fromCharCode(buf[o], buf[o + 1], buf[o + 2], buf[o + 3]);
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') throw new Error('not WAV');
  let pos = 12, fmt = null, dataOff = -1, dataLen = 0;
  while (pos + 8 <= dv.byteLength) {
    const id = tag(pos), size = dv.getUint32(pos + 4, true), body = pos + 8;
    if (id === 'fmt ') {
      let f = dv.getUint16(body, true);
      const ch = dv.getUint16(body + 2, true), sr = dv.getUint32(body + 4, true), bits = dv.getUint16(body + 14, true);
      if (f === 0xfffe && size >= 40) f = dv.getUint16(body + 24, true);
      fmt = { f, ch, sr, bits };
    } else if (id === 'data') { dataOff = body; dataLen = size; }
    pos = body + size + (size & 1);
  }
  if (!fmt || dataOff < 0) throw new Error('bad WAV');
  const bps = fmt.bits >> 3, frames = Math.floor(dataLen / (bps * fmt.ch));
  const channels = Array.from({ length: fmt.ch }, () => new Float32Array(frames));
  const isFloat = fmt.f === 3;
  for (let fr = 0; fr < frames; fr++) {
    for (let c = 0; c < fmt.ch; c++) {
      const o = dataOff + (fr * fmt.ch + c) * bps;
      channels[c][fr] = readSample(dv, o, fmt.bits, isFloat);
    }
  }
  return { sampleRate: fmt.sr, channels };
}
function readSample(dv, o, bits, isFloat) {
  if (isFloat) return bits === 64 ? dv.getFloat64(o, true) : dv.getFloat32(o, true);
  switch (bits) {
    case 8: return (dv.getUint8(o) - 128) / 128;
    case 16: return dv.getInt16(o, true) / 32768;
    case 24: { let v = dv.getUint8(o) | (dv.getUint8(o + 1) << 8) | (dv.getUint8(o + 2) << 16); if (v & 0x800000) v |= ~0xffffff; return v / 8388608; }
    case 32: return dv.getInt32(o, true) / 2147483648;
    default: throw new Error('bits ' + bits);
  }
}

// ---- WAV encode (16-bit stereo) --------------------------------------------
function encodeWavStereo(left, right, sampleRate) {
  const frames = left.length;
  const dataLen = frames * 2 * 2;
  const buf = Buffer.alloc(44 + dataLen);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + dataLen, 4); buf.write('WAVE', 8);
  buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(2, 22); buf.writeUInt32LE(sampleRate, 24);
  buf.writeUInt32LE(sampleRate * 4, 28); buf.writeUInt16LE(4, 32); buf.writeUInt16LE(16, 34);
  buf.write('data', 36); buf.writeUInt32LE(dataLen, 40);
  let o = 44;
  for (let i = 0; i < frames; i++) {
    buf.writeInt16LE(toI16(left[i]), o); o += 2;
    buf.writeInt16LE(toI16(right[i]), o); o += 2;
  }
  return buf;
}
const toI16 = (x) => { let v = Math.round(Math.max(-1, Math.min(1, x)) * 32767); return v; };

function routeForChannelName(name) {
  const last = name.charAt(name.length - 1);
  if (last === 'L' || /left/i.test(name)) return ROUTE_LEFT;
  if (last === 'R' || /right/i.test(name)) return ROUTE_RIGHT;
  return ROUTE_BOTH;
}

// ---- premix one sample to stereo (mirrors Voice::start routing) ------------
function premixSample(audiofiles, instDir) {
  // audiofiles: [{file, filechannel(1-based), channelName}]
  const decodedByFile = new Map();
  const getDec = (rel) => {
    const p = path.join(instDir, rel);
    if (!decodedByFile.has(p)) decodedByFile.set(p, decodeWav(fs.readFileSync(p)));
    return decodedByFile.get(p);
  };

  let maxLen = 0, sampleRate = 44100;
  const parts = [];
  let numBoth = 0;
  for (const a of audiofiles) {
    const dec = getDec(a.file);
    sampleRate = dec.sampleRate;
    const ch = dec.channels[Math.min(a.filechannel - 1, dec.channels.length - 1)];
    if (!ch) continue;
    const routing = routeForChannelName(a.channelName);
    if (routing === ROUTE_BOTH) numBoth++;
    parts.push({ ch, routing });
    maxLen = Math.max(maxLen, ch.length);
  }

  const bothGain = numBoth > 1 ? 1 / Math.sqrt(numBoth) : 1;
  const L = new Float32Array(maxLen), R = new Float32Array(maxLen);
  for (const p of parts) {
    const g = p.routing === ROUTE_BOTH ? bothGain : 1;
    const n = p.ch.length;
    if (p.routing === ROUTE_LEFT) for (let i = 0; i < n; i++) L[i] += p.ch[i] * g;
    else if (p.routing === ROUTE_RIGHT) for (let i = 0; i < n; i++) R[i] += p.ch[i] * g;
    else for (let i = 0; i < n; i++) { const v = p.ch[i] * g; L[i] += v; R[i] += v; }
  }
  return { L, R, sampleRate };
}

// Trim trailing near-silence (keep a short release tail) and optionally cap
// total duration, applying a short fade-out so the cut is inaudible.
function trimStereo(L, R, sampleRate) {
  const thresh = 3e-4;
  let end = L.length;
  while (end > 1 && Math.abs(L[end - 1]) < thresh && Math.abs(R[end - 1]) < thresh) end--;
  end = Math.min(L.length, end + 256); // small tail

  if (MAX_DUR > 0) {
    const cap = Math.floor(MAX_DUR * sampleRate);
    if (end > cap) end = cap;
  }
  let l = L.slice(0, end), r = R.slice(0, end);

  // Fade the last 20ms so a duration cap doesn't click.
  const fade = Math.min(l.length, Math.floor(0.02 * sampleRate));
  for (let i = 0; i < fade; i++) {
    const g = (fade - i) / fade;
    const idx = l.length - fade + i;
    l[idx] *= g; r[idx] *= g;
  }
  return { L: l, R: r };
}

// ---- select a spread of layers/round-robin variants ------------------------
function selectSamples(samples) {
  // Group by power, keep up to RR_PER_LAYER per power, then subsample distinct
  // powers down to MAX_LAYERS spread across the dynamic range.
  const byPower = new Map();
  for (const s of samples) {
    const key = s.power;
    if (!byPower.has(key)) byPower.set(key, []);
    const arr = byPower.get(key);
    if (arr.length < RR_PER_LAYER) arr.push(s);
  }
  const powers = [...byPower.keys()].sort((a, b) => a - b);
  let chosenPowers = powers;
  if (powers.length > MAX_LAYERS) {
    chosenPowers = [];
    for (let i = 0; i < MAX_LAYERS; i++) {
      const idx = Math.round((i / (MAX_LAYERS - 1)) * (powers.length - 1));
      chosenPowers.push(powers[idx]);
    }
    chosenPowers = [...new Set(chosenPowers)];
  }
  const out = [];
  for (const p of chosenPowers) out.push(...byPower.get(p));
  return out;
}

// ---- main ------------------------------------------------------------------
const kitPath = path.join(kitDir, kitXml);
const kitDoc = fs.readFileSync(kitPath, 'utf8');
const drumkitTag = tags(kitDoc, 'drumkit')[0] || '';
const kitDisplayName = KIT_NAME || attr(drumkitTag, 'name') || 'Kit';
const kitDescription = attr(drumkitTag, 'description') || '';

// midimap
const midiMapSrc = attr((tags(kitDoc, 'defaultmidimap')[0] || ''), 'src')
  || kitXml.replace(/\.xml$/i, '') + '_midimap.xml';
let midimap = [];
try {
  const mm = fs.readFileSync(path.join(kitDir, midiMapSrc), 'utf8');
  midimap = tags(mm, 'map').map((a) => ({ note: parseInt(attr(a, 'note'), 10), instr: attr(a, 'instr') }));
} catch (e) { console.warn('No midimap:', midiMapSrc); }

fs.rmSync(outDir, { recursive: true, force: true });
fs.mkdirSync(outDir, { recursive: true });

// First pass: premix + trim everything, collecting a global peak for normalization.
const instrOut = [];
let globalPeak = 0;

const instrumentTags = tags(kitDoc, 'instrument');
for (const it of instrumentTags) {
  const name = attr(it, 'name');
  const file = attr(it, 'file');
  if (!name || !file) continue;
  const instPath = path.join(kitDir, file);
  let instDoc;
  try { instDoc = fs.readFileSync(instPath, 'utf8'); } catch (e) { console.warn('skip', name); continue; }
  const instDir = path.dirname(instPath);

  const allSamples = blocks(instDoc, 'sample').map((b) => {
    const power = parseFloat(attr(b.attrs, 'power')) || 1;
    const audiofiles = tags(b.body, 'audiofile').map((a) => ({
      file: attr(a, 'file'), filechannel: parseInt(attr(a, 'filechannel') || '1', 10), channelName: attr(a, 'channel') || '',
    }));
    return { power, audiofiles };
  });

  const chosen = selectSamples(allSamples);
  const premixed = [];
  for (const s of chosen) {
    try {
      const mix = premixSample(s.audiofiles, instDir);
      const t = trimStereo(mix.L, mix.R, mix.sampleRate);
      for (let i = 0; i < t.L.length; i++) { globalPeak = Math.max(globalPeak, Math.abs(t.L[i]), Math.abs(t.R[i])); }
      premixed.push({ power: s.power, L: t.L, R: t.R, sampleRate: mix.sampleRate });
    } catch (e) { console.warn('  premix fail', name, e.message); }
  }
  if (premixed.length) instrOut.push({ name, samples: premixed });
  process.stdout.write(`\r  ${instrOut.length}/${instrumentTags.length} instruments...   `);
}
process.stdout.write('\n');

// Normalize the whole kit to peak 0.9 (uniform gain preserves relative dynamics).
const norm = globalPeak > 0 ? 0.9 / globalPeak : 1;
console.log(`global peak ${globalPeak.toFixed(4)} -> normalize x${norm.toFixed(3)}`);

// Second pass: normalize powers per instrument to [0,1] and write files.
const manifest = { name: kitDisplayName, description: kitDescription, sampleRate: 44100, instruments: [], midimap: [] };
let totalBytes = 0, totalFiles = 0;

for (const inst of instrOut) {
  const dir = path.join(outDir, safeName(inst.name));
  fs.mkdirSync(dir, { recursive: true });
  // Normalize power values within the instrument to 0..1 for the engine.
  let minP = Infinity, maxP = -Infinity;
  for (const s of inst.samples) { minP = Math.min(minP, s.power); maxP = Math.max(maxP, s.power); }
  const normPower = (p) => (maxP > minP ? (p - minP) / (maxP - minP) : 1);

  const outSamples = [];
  inst.samples.forEach((s, i) => {
    const L = Float32Array.from(s.L, (x) => x * norm);
    const R = Float32Array.from(s.R, (x) => x * norm);
    const wav = encodeWavStereo(L, R, s.sampleRate);
    const fname = safeName(inst.name) + '/' + String(i).padStart(4, '0') + '.wav';
    fs.writeFileSync(path.join(outDir, fname), wav);
    totalBytes += wav.length; totalFiles++;
    outSamples.push({ power: normPower(s.power), file: fname });
  });
  manifest.instruments.push({ name: inst.name, samples: outSamples });
}

// Keep only midimap entries whose instrument survived.
const kept = new Set(manifest.instruments.map((i) => i.name));
manifest.midimap = midimap.filter((m) => kept.has(m.instr));

fs.writeFileSync(path.join(outDir, 'kit.json'), JSON.stringify(manifest, null, 1));
console.log(`Wrote ${totalFiles} samples across ${manifest.instruments.length} instruments, ${(totalBytes / 1048576).toFixed(1)} MB -> ${outDir}`);

function safeName(n) { return n.replace(/[^A-Za-z0-9_.-]/g, '_'); }
