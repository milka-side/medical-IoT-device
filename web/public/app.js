const DISPLAY_SECONDS = 5; // how much of the trace is drawn on screen
const SCALE_SECONDS = 20; // how much history the Y-axis autoscale looks at -
// long enough to stay stable beat-to-beat, short enough that the scale
// still adapts within a few beats once contact quality changes, instead of
// a stale spike from a while ago keeping everything looking tiny/flat.
const ALARMS = {
  hr: (v) => v < 50 || v > 120,
  spo2: (v) => v < 92,
  temp: (v) => v < 35 || v > 37.8,
};

const STALE_MS = 5000;

const MEASURE_LABELS = {
  ecg: 'Виміряти ЕКГ',
  vitals: 'Виміряти пульс/SpO₂',
  temp: 'Виміряти температуру',
  all: 'Виміряти все',
};
const REMAINING_KEY = { ecg: 'ecg_remaining_s', vitals: 'vitals_remaining_s', temp: 'temp_remaining_s' };

const $ = (id) => document.getElementById(id);
const canvas = $('ecg');
const banner = $('banner');
const measureButtons = [...document.querySelectorAll('.measure-btn')];

let fs = 250;
let ecg = [];       // last DISPLAY_SECONDS, drawn as the line
let ecgScale = [];  // last SCALE_SECONDS, used only to compute lo/hi
let scaleLo = -1, scaleHi = 1; // cached, recomputed on new data (not every frame)
let lastDataAt = 0;
let wsOpen = false;
let deviceId = null;
const lastMeasuredAt = { hr: 0, spo2: 0, temp: 0, ecg: 0 }; // epoch ms, 0 = never measured
let lastAdcSps = 0; // ADS1293 samples/s actually delivered, ~853 when nothing is dropped

// Robust autoscale: ignore only the most extreme ~0.2% of samples (a 1-2
// sample motion spike), then pad the range. The previous 3%/97% cut
// discarded far more than outliers - an R peak is only ~2-4% of a cardiac
// cycle's samples, so it got clamped flat at roughly half its true height
// while T waves and noise were stretched up to the same size, which is why
// real traces looked like noise with peaks "sometimes there".
const SCALE_TRIM = 0.002;
const SCALE_PAD = 0.1;

function percentileRange(arr, p) {
  const sorted = [...arr].sort((a, b) => a - b);
  const lo = sorted[Math.floor(p * (sorted.length - 1))];
  const hi = sorted[Math.floor((1 - p) * (sorted.length - 1))];
  return [lo, hi];
}

function fmtMMSS(seconds) {
  const s = Math.max(0, Math.round(seconds));
  const m = Math.floor(s / 60);
  return `${m}:${String(s % 60).padStart(2, '0')}`;
}

// Values persist once measured - they never revert to "--" just because
// the window closed, so a nurse glancing at the screen later still sees
// the last reading instead of a blank tile. The "measured at" line is what
// tells them it's not live right now.
function showVitals(d) {
  for (const [key, digits] of [['hr', 0], ['spo2', 0], ['temp', 1]]) {
    if (d[key] === undefined) continue; // no fresh reading this message - keep showing the last one
    $(key).textContent = d[key].toFixed(digits);
    $('tile-' + key).classList.toggle('alarm', ALARMS[key](d[key]));
    lastMeasuredAt[key] = d.received_at || Date.now();
  }
  refreshAges();
}

function fmtAgo(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 5) return 'щойно';
  if (s < 60) return `${s}с тому`;
  const m = Math.round(s / 60);
  if (m < 60) return `${m}хв тому`;
  return `${Math.round(m / 60)}год тому`;
}

function refreshAges() {
  const now = Date.now();
  for (const key of ['hr', 'spo2', 'temp', 'ecg']) {
    const el = $(key + '-age');
    if (!el) continue;
    if (!lastMeasuredAt[key]) {
      el.textContent = '';
      continue;
    }
    const time = new Date(lastMeasuredAt[key]).toLocaleTimeString('uk-UA', {
      hour: '2-digit', minute: '2-digit', second: '2-digit',
    });
    // Well under ~853 means the device is missing ADC samples, which
    // distorts the trace's time axis - worth seeing at a glance.
    const rate = key === 'ecg' && lastAdcSps ? ` · АЦП ${lastAdcSps} Гц` : '';
    el.textContent = `о ${time} (${fmtAgo(now - lastMeasuredAt[key])})${rate}`;
  }
}

// ERROR_LOD from the ADS1293: bit0 = RA, bit1 = LA electrode without skin
// contact. Only present while ECG is recording.
function showLeadOff(d) {
  const el = $('ecg-lod');
  if (d.lod === undefined) {
    el.hidden = true;
    return;
  }
  const off = [];
  if (d.lod & 1) off.push('RA (права)');
  if (d.lod & 2) off.push('LA (ліва)');
  el.hidden = off.length === 0;
  el.textContent = `Немає контакту електрода: ${off.join(', ')} — перевір, чи щільно прилягає до шкіри`;
}

// Boot-lockout banner + per-button labels/disabled state, driven entirely
// by the status/*_remaining_s fields the firmware always sends.
function updateControls(d) {
  const lockout = d.status === 'boot_lockout';
  if (lockout) {
    banner.hidden = false;
    banner.className = 'banner lockout';
    banner.textContent = `Пристрій щойно увімкнено — вимірювання стануть доступні через ${fmtMMSS(d.lockout_remaining_s ?? 0)}`;
  } else {
    banner.hidden = true;
  }

  for (const btn of measureButtons) {
    const type = btn.dataset.type;
    if (lockout) {
      btn.disabled = true;
      btn.classList.remove('active');
      btn.textContent = MEASURE_LABELS[type];
      continue;
    }
    if (type === 'all') {
      const anyActive = ['ecg', 'vitals', 'temp'].some((k) => d[REMAINING_KEY[k]] !== undefined);
      btn.disabled = false;
      btn.classList.toggle('active', anyActive);
      btn.textContent = MEASURE_LABELS.all;
      continue;
    }
    const remaining = d[REMAINING_KEY[type]];
    if (remaining === undefined) {
      btn.disabled = false;
      btn.classList.remove('active');
      btn.textContent = MEASURE_LABELS[type];
    } else {
      btn.disabled = true;
      btn.classList.add('active');
      btn.textContent = `Вимірюю… ${fmtMMSS(remaining)}`;
    }
  }
}

function addEcg(samples, rate) {
  fs = rate || fs;

  ecg.push(...samples);
  const max = fs * DISPLAY_SECONDS;
  if (ecg.length > max) ecg = ecg.slice(-max);

  ecgScale.push(...samples);
  const smax = fs * SCALE_SECONDS;
  if (ecgScale.length > smax) ecgScale = ecgScale.slice(-smax);

  if (ecgScale.length >= 2) {
    const [lo, hi] = percentileRange(ecgScale, SCALE_TRIM);
    const pad = (hi - lo) * SCALE_PAD;
    scaleLo = lo - pad;
    scaleHi = hi + pad;
  }
}

function drawEcg() {
  const dpr = window.devicePixelRatio || 1;
  const w = (canvas.width = canvas.clientWidth * dpr);
  const h = (canvas.height = canvas.clientHeight * dpr);
  const ctx = canvas.getContext('2d');

  // Same persistence rule as the vital tiles: a finished measurement keeps
  // showing its last trace (frozen) instead of going blank - the
  // ecg-age label under the canvas is what says it's not live anymore.
  if (ecg.length < 2) {
    ctx.fillStyle = getComputedStyle(document.documentElement).getPropertyValue('--muted');
    ctx.font = `${14 * dpr}px system-ui, -apple-system, sans-serif`;
    ctx.fillText('ЕКГ ще не вимірювалась', 16 * dpr, h / 2);
    return;
  }

  const lo = scaleLo, hi = scaleHi;
  const span = hi - lo || 1;
  const n = fs * DISPLAY_SECONDS;

  ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue('--hr');
  ctx.lineWidth = 2 * dpr;
  ctx.beginPath();
  ecg.forEach((v, i) => {
    const x = (i / (n - 1)) * w;
    const yClamped = Math.max(lo, Math.min(hi, v));
    const y = h * 0.9 - ((yClamped - lo) / span) * h * 0.8;
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.stroke();
}

const conn = $('conn');

function noteData(receivedAt) {
  if (receivedAt && receivedAt > lastDataAt) lastDataAt = receivedAt;
}

function updateConnBadge() {
  if (!wsOpen) {
    conn.textContent = 'не підключено';
    conn.className = 'conn off';
    return;
  }
  if (lastDataAt && Date.now() - lastDataAt > STALE_MS) {
    const sec = Math.round((Date.now() - lastDataAt) / 1000);
    conn.textContent = `немає даних (${sec}с)`;
    conn.className = 'conn warn';
    return;
  }
  conn.textContent = 'підключено';
  conn.className = 'conn on';
}

function handleDeviceMessage(d) {
  if (d.device_id) deviceId = d.device_id;
  showVitals(d);
  updateControls(d);
  showLeadOff(d);
  noteData(d.received_at);
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { wsOpen = true; updateConnBadge(); };
  ws.onclose = () => {
    wsOpen = false;
    updateConnBadge();
    setTimeout(connect, 2000);
  };
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.type === 'snapshot') {
      Object.values(m.devices).forEach(handleDeviceMessage);
    }
    if (m.type === 'data') {
      handleDeviceMessage(m.data);
      if (m.data.ecg) {
        addEcg(m.data.ecg, m.data.fs);
        lastMeasuredAt.ecg = m.data.received_at || Date.now();
        if (m.data.adc_sps) lastAdcSps = Math.round(m.data.adc_sps);
        refreshAges();
      }
    }
  };
}

for (const btn of measureButtons) {
  btn.addEventListener('click', async () => {
    if (!deviceId || btn.disabled) return;
    try {
      const r = await fetch('/api/command', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device_id: deviceId, type: btn.dataset.type }),
      });
      if (!r.ok) console.error('сервер відповів', r.status, await r.text());
    } catch (e) {
      console.error('не вдалося надіслати команду:', e.message);
    }
  });
}

connect();
setInterval(updateConnBadge, 1000);
setInterval(refreshAges, 1000); // keeps "X тому" live between messages, not just on arrival
(function loop() {
  drawEcg();
  requestAnimationFrame(loop);
})();
