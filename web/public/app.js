// DrumCraker web demo — UI wiring.
'use strict';

const engine = new DrumCrakerEngine();
let currentKit = null;

const KEYS = '1234567890qwertyuiopasdfghjklzxcvbnm';

// ---- helpers ---------------------------------------------------------------
const $ = (id) => document.getElementById(id);

function groupOf(name) {
  const n = name.toLowerCase();
  if (/kick|kdrum/.test(n)) return 'kick';
  if (/snare/.test(n)) return 'snare';
  if (/hihat|hh/.test(n)) return 'hihat';
  if (/tom|floor/.test(n)) return 'tom';
  if (/crash|ride|cym|china|splash|bell/.test(n)) return 'cym';
  return 'other';
}

// ---- parameters ------------------------------------------------------------
function pushParams() {
  engine.setParams({
    db: parseFloat($('pMaster').value),
    vel: parseFloat($('pVel').value),
    tim: parseFloat($('pTim').value),
    rr: parseFloat($('pRR').value),
  });
}

function bindParams() {
  const bind = (id, out, fmt) => {
    const el = $(id), o = $(out);
    const update = () => { o.textContent = fmt(parseFloat(el.value)); pushParams(); };
    el.addEventListener('input', update);
    update();
  };
  bind('pMaster', 'vMaster', (v) => v.toFixed(1) + ' dB');
  bind('pVel', 'vVel', (v) => Math.round(v * 100) + '%');
  bind('pTim', 'vTim', (v) => v.toFixed(1) + ' ms');
  bind('pRR', 'vRR', (v) => v.toFixed(2));
  const pv = $('padVel'), pvo = $('vPadVel');
  pv.addEventListener('input', () => (pvo.textContent = parseFloat(pv.value).toFixed(2)));
}

// ---- pads ------------------------------------------------------------------
const padByNote = new Map();

function buildPads(kit) {
  const wrap = $('pads');
  wrap.innerHTML = '';
  padByNote.clear();
  const playable = kit.instruments.filter((i) => i.note !== null);
  $('padHint').textContent = `${playable.length} instruments · click a pad or use your keyboard`;

  playable.forEach((inst, i) => {
    const pad = document.createElement('div');
    pad.className = 'pad';
    pad.dataset.group = groupOf(inst.name);
    pad.dataset.note = inst.note;
    const key = i < KEYS.length ? KEYS[i] : '';
    pad.innerHTML =
      `<div class="glow"></div>` +
      (key ? `<div class="key">${key.toUpperCase()}</div>` : '') +
      `<div class="name">${inst.name}</div>` +
      `<div class="meta">note ${inst.note} · ${inst.layers} layer${inst.layers > 1 ? 's' : ''}</div>`;

    pad.addEventListener('pointerdown', (e) => {
      // Vertical click position controls velocity (top = loud).
      const rect = pad.getBoundingClientRect();
      const rel = 1 - (e.clientY - rect.top) / rect.height;
      const vel = Math.max(0.08, Math.min(1, 0.25 + rel * 0.75));
      hit(inst.note, vel);
    });

    padByNote.set(inst.note, pad);
    if (key) keyToNote.set(key, inst.note);
    wrap.appendChild(pad);
  });
}

function flashPad(note) {
  const pad = padByNote.get(note);
  if (!pad) return;
  pad.classList.remove('hit');
  void pad.offsetWidth; // restart transition
  pad.classList.add('hit');
  setTimeout(() => pad.classList.remove('hit'), 160);
}

function hit(note, velocity) {
  engine.noteOn(note, velocity);
  flashPad(note);
}

// ---- keyboard --------------------------------------------------------------
const keyToNote = new Map();
const heldKeys = new Set();

window.addEventListener('keydown', (e) => {
  if (e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
  if (e.code === 'Space') { e.preventDefault(); toggleSequencer(); return; }
  const k = e.key.toLowerCase();
  if (keyToNote.has(k) && !heldKeys.has(k)) {
    heldKeys.add(k);
    hit(keyToNote.get(k), parseFloat($('padVel').value));
  }
});
window.addEventListener('keyup', (e) => heldKeys.delete(e.key.toLowerCase()));

// ---- sequencer -------------------------------------------------------------
const STEPS = 16;
let seqRows = [];       // [{name, note, steps:[bool x16], accents:[bool]}]
let seqPlaying = false;
let currentStep = 0;
let nextStepTime = 0;
let schedulerTimer = null;

// Pick a representative instrument note for a role by name pattern.
function findNote(kit, patterns) {
  for (const p of patterns) {
    const inst = kit.instruments.find((i) => i.note !== null && p.test(i.name));
    if (inst) return { name: inst.name, note: inst.note };
  }
  return null;
}

function buildSeqRows(kit) {
  const roles = [
    { label: 'Kick', patterns: [/^kick(?!.*(edg|rim))/, /kick/] },
    { label: 'Snare', patterns: [/^snare_supr64$/, /^snare(?!.*(edg|rim|ss))/, /snare/] },
    { label: 'Closed HH', patterns: [/hihat.*clo(?!.*edg)/, /hihat.*clo/, /hihat/] },
    { label: 'Open HH', patterns: [/hihat.*op1(?!.*edg)/, /hihat.*op/, /hihat.*spl/] },
    { label: 'Tom', patterns: [/tom.*16(?!.*edg)/, /tom(?!.*edg)/, /tom/] },
    { label: 'Ride', patterns: [/ride.*bow/, /ride(?!.*mute)/, /ride/] },
    { label: 'Crash', patterns: [/crash.*bow/, /crash(?!.*mute)/, /crash/, /cym/] },
  ];
  seqRows = [];
  for (const r of roles) {
    const found = findNote(kit, r.patterns);
    if (found) seqRows.push({ label: r.label, name: found.name, note: found.note, steps: new Array(STEPS).fill(false) });
  }
}

function renderSeq() {
  const seq = $('seq');
  seq.innerHTML = '';
  seqRows.forEach((row, ri) => {
    const el = document.createElement('div');
    el.className = 'seq-row';
    const steps = document.createElement('div');
    steps.className = 'steps';
    row.steps.forEach((on, si) => {
      const s = document.createElement('div');
      s.className = 'step' + (on ? ' on' : '');
      s.addEventListener('pointerdown', () => {
        row.steps[si] = !row.steps[si];
        s.classList.toggle('on', row.steps[si]);
        if (row.steps[si]) hit(row.note, 0.85);
      });
      steps.appendChild(s);
    });
    el.innerHTML = `<div class="label" title="${row.name}">${row.label}</div>`;
    el.appendChild(steps);
    seq.appendChild(el);
  });
}

const PATTERNS = {
  empty: {},
  fourfloor: {
    Kick: [0, 4, 8, 12], 'Closed HH': [0, 2, 4, 6, 8, 10, 12, 14], Snare: [4, 12], 'Open HH': [2, 6, 10, 14],
  },
  rock: {
    Kick: [0, 6, 8, 10], Snare: [4, 12], 'Closed HH': [0, 2, 4, 6, 8, 10, 12, 14], Crash: [0],
  },
  halftime: {
    Kick: [0, 10], Snare: [8], 'Closed HH': [0, 2, 4, 6, 8, 10, 12, 14], Ride: [0, 4, 8, 12],
  },
};

function applyPattern(name) {
  const p = PATTERNS[name] || {};
  for (const row of seqRows) {
    row.steps.fill(false);
    const hits = p[row.label];
    if (hits) for (const s of hits) if (s < STEPS) row.steps[s] = true;
  }
  renderSeq();
}

function clearSteps() { for (const row of seqRows) row.steps.fill(false); renderSeq(); }

function stepDuration() {
  const bpm = parseFloat($('tempo').value);
  return 60 / bpm / 4; // 16th notes
}

function scheduler() {
  const lookahead = 0.1;
  while (nextStepTime < engine.now() + lookahead) {
    const step = currentStep;
    for (const row of seqRows) {
      if (row.steps[step]) {
        const vel = step % 4 === 0 ? 0.95 : 0.8; // accent downbeats
        engine.noteAt(row.note, vel, nextStepTime);
      }
    }
    scheduleUiFlash(step, nextStepTime);
    nextStepTime += stepDuration();
    currentStep = (currentStep + 1) % STEPS;
  }
}

function scheduleUiFlash(step, when) {
  const delay = Math.max(0, (when - engine.now()) * 1000);
  setTimeout(() => {
    document.querySelectorAll('.step.playhead').forEach((s) => s.classList.remove('playhead'));
    seqRows.forEach((row, ri) => {
      const stepEl = $('seq').children[ri]?.querySelector('.steps').children[step];
      if (stepEl) stepEl.classList.add('playhead');
      if (row.steps[step]) flashPad(row.note);
    });
  }, delay);
}

function toggleSequencer() {
  seqPlaying ? stopSequencer() : startSequencer();
}
function startSequencer() {
  if (!currentKit) return;
  seqPlaying = true;
  currentStep = 0;
  nextStepTime = engine.now() + 0.06;
  schedulerTimer = setInterval(scheduler, 25);
  const b = $('seqPlay'); b.textContent = '■ Stop'; b.classList.add('playing');
}
function stopSequencer() {
  seqPlaying = false;
  clearInterval(schedulerTimer);
  document.querySelectorAll('.step.playhead').forEach((s) => s.classList.remove('playhead'));
  const b = $('seqPlay'); b.textContent = '▶ Play'; b.classList.remove('playing');
}

// ---- meter -----------------------------------------------------------------
function meterLoop() {
  const level = engine.getLevel();
  $('meterFill').style.width = Math.min(100, level * 140) + '%';
  const n = engine.voiceCount || 0;
  $('voiceCount').textContent = n + (n === 1 ? ' voice' : ' voices');
  requestAnimationFrame(meterLoop);
}

// ---- kit loading -----------------------------------------------------------
function showLoad(show) { $('loadBar').classList.toggle('hidden', !show); }

engine.onProgress = (done, total) => {
  const pct = total ? Math.round((done / total) * 100) : 0;
  $('loadFill').style.width = pct + '%';
  $('loadText').textContent = `Decoding samples… ${done}/${total}`;
};

async function loadKit(loader) {
  stopSequencer();
  showLoad(true);
  $('kitInfo').textContent = 'loading…';
  try {
    await loader();
    onKitLoaded(engine.kit);
  } catch (err) {
    console.error(err);
    $('kitInfo').textContent = 'load failed: ' + err.message;
    alert('Failed to load kit: ' + err.message);
  } finally {
    showLoad(false);
  }
}

function onKitLoaded(kit) {
  currentKit = kit;
  const playable = kit.instruments.filter((i) => i.note !== null).length;
  $('kitInfo').textContent = `${kit.name} — ${playable} instruments, ${kit.bufferCount} samples`;
  buildPads(kit);
  buildSeqRows(kit);
  applyPattern($('patternSelect').value === 'empty' ? 'rock' : $('patternSelect').value);
  if ($('patternSelect').value === 'empty') $('patternSelect').value = 'rock';
  pushParams();
}

// ---- boot ------------------------------------------------------------------
async function start() {
  $('startBtn').disabled = true;
  $('startBtn').textContent = 'Loading…';
  await engine.init();
  await engine.resume();
  bindParams();
  meterLoop();

  await loadKit(() => engine.loadWebKit('kits/tchackpoum'));
  $('overlay').classList.add('hidden');
}

$('startBtn').addEventListener('click', start);

$('kitSelect').addEventListener('change', (e) => {
  const val = e.target.value;
  if (val) loadKit(() => engine.loadWebKit(val));
});

$('loadFolderBtn').addEventListener('click', () => $('folderInput').click());
$('folderInput').addEventListener('change', (e) => {
  const files = e.target.files;
  if (files && files.length) loadKit(() => engine.loadKitFromFiles(files));
});

$('seqPlay').addEventListener('click', toggleSequencer);
$('seqClear').addEventListener('click', clearSteps);
$('patternSelect').addEventListener('change', (e) => applyPattern(e.target.value));
$('tempo').addEventListener('input', () => ($('vTempo').textContent = $('tempo').value));
