const DISPLAY_SECONDS = 5; // how much of the trace is drawn on screen
const SCALE_SECONDS = 20; // how much history the Y-axis autoscale looks at -
// long enough to stay stable beat-to-beat, short enough that the scale
// still adapts within a few beats once contact quality changes, instead of
// a stale spike from a while ago keeping everything looking tiny/flat.
const ALARMS = {
  hr: (v) => v < 50 || v > 120,
  spo2: (v) => v < 92,
  temp: (v) => v < 35 || v > 37.8, // applied to the estimated body temperature
};

// Through Azure the device sends one message a second only while ECG is
// recording; otherwise one status a minute (F1 quota) plus an immediate one
// on every change. So silence means "gone" after a few seconds during ECG,
// but only after 90 s otherwise.
const STALE_MS = 90 * 1000;
const STALE_ECG_MS = 5000;
let lastEcgActive = false;

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
let lastTempBody; // estimated body temperature for the last temperature result
let lastAdcSps = 0; // ADS1293 samples/s actually delivered, ~800 when nothing is dropped
// Seconds left in the pulse/SpO2 and temperature windows (undefined = not
// measuring). Those values only arrive once, as the 3-minute average, when
// the window ends - meanwhile the tile says when the result will be ready.
let measuring = {};

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
// The temperature tile shows the measured fingertip temperature; the line
// under it carries the device's estimate of body temperature, and the
// alarm colour follows that estimate.
function showVitals(d) {
  for (const [key, digits] of [['hr', 0], ['spo2', 0], ['temp', 1]]) {
    if (d[key] === undefined) continue; // no fresh reading this message - keep showing the last one
    $(key).textContent = d[key].toFixed(digits);
    lastMeasuredAt[key] = d.received_at || Date.now();
    if (key === 'temp') {
      lastTempBody = d.temp_body;
      $('tile-temp').classList.toggle('alarm', lastTempBody !== undefined && ALARMS.temp(lastTempBody));
    } else {
      $('tile-' + key).classList.toggle('alarm', ALARMS[key](d[key]));
    }
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
    const left = key === 'temp' ? measuring.temp : key === 'ecg' ? undefined : measuring.vitals;
    if (left !== undefined) {
      el.textContent = `вимірюю — середнє за 3 хв буде через ${fmtMMSS(left)}`;
      continue;
    }
    if (!lastMeasuredAt[key]) {
      el.textContent = '';
      continue;
    }
    const time = new Date(lastMeasuredAt[key]).toLocaleTimeString('uk-UA', {
      hour: '2-digit', minute: '2-digit', second: '2-digit',
    });
    // Well under ~800 means the device is missing ADC samples, which
    // distorts the trace's time axis - worth seeing at a glance.
    const rate = key === 'ecg' && lastAdcSps ? ` · АЦП ${lastAdcSps} Гц` : '';
    const body = key === 'temp' && lastTempBody !== undefined
      ? ` · орієнтовна температура тіла ≈ ${lastTempBody.toFixed(1)} °C` : '';
    el.textContent = `о ${time} (${fmtAgo(now - lastMeasuredAt[key])})${rate}${body}`;
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
  // Warm-up after power-on: normal buttons wait, "Виміряти все одно"
  // (all_force) starts a measurement anyway. A forced measurement can be
  // running while the warm-up countdown is still going.
  const lockout = (d.lockout_remaining_s ?? 0) > 0;
  const anyActive = ['ecg', 'vitals', 'temp'].some((k) => d[REMAINING_KEY[k]] !== undefined);
  if (lockout) {
    banner.hidden = false;
    banner.className = 'banner lockout';
    $('banner-text').textContent = `Датчики прогріваються після ввімкнення — точні вимірювання будуть доступні через ${fmtMMSS(d.lockout_remaining_s)}`;
    $('btn-force').hidden = false; // usable for the whole warm-up, even mid-measurement
  } else {
    banner.hidden = true;
  }
  measuring = {
    vitals: d.vitals_remaining_s, temp: d.temp_remaining_s,
  };
  // While a window runs its result doesn't exist yet - show that instead of
  // the previous measurement's number.
  if (measuring.vitals !== undefined) {
    for (const k of ['hr', 'spo2']) { $(k).textContent = '…'; $('tile-' + k).classList.remove('alarm'); }
  }
  if (measuring.temp !== undefined) { $('temp').textContent = '…'; $('tile-temp').classList.remove('alarm'); }

  for (const btn of measureButtons) {
    const type = btn.dataset.type;
    if (lockout) {
      btn.disabled = true;
      btn.classList.remove('active');
      btn.textContent = MEASURE_LABELS[type];
      continue;
    }
    if (type === 'all') {
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

function noteData(receivedAt, ecgActive) {
  if (receivedAt && receivedAt > lastDataAt) {
    lastDataAt = receivedAt;
    lastEcgActive = ecgActive;
  }
}

function updateConnBadge() {
  if (!wsOpen) {
    conn.textContent = 'не підключено';
    conn.className = 'conn off';
    return;
  }
  const stale = lastEcgActive ? STALE_ECG_MS : STALE_MS;
  if (lastDataAt && Date.now() - lastDataAt > stale) {
    const sec = Math.round((Date.now() - lastDataAt) / 1000);
    conn.textContent = `немає даних (${sec < 120 ? `${sec} с` : `${Math.round(sec / 60)} хв`})`;
    conn.className = 'conn warn';
    return;
  }
  conn.textContent = 'підключено';
  conn.className = 'conn on';
}

// "Завантажити дані з SD": enabled only while the device reports saved
// files on its card and isn't measuring (it can't read the card then).
const sdBtn = $('btn-sd');
let sdFiles = 0;
let sdUploadUntil = 0; // button held disabled after a press until the device reports back

function updateSdButton(d) {
  if (d.sd_files !== undefined) {
    if (sdUploadUntil && d.sd_files < sdFiles) {
      showNotice(d.sd_files ? `Передано, на картці ще файлів: ${d.sd_files}.` : 'Усі дані з SD-картки передано й збережено в записах. Картку очищено.');
      if (!d.sd_files) sdUploadUntil = 0;
    }
    sdFiles = d.sd_files;
  }
  const measuring = ['ecg', 'vitals', 'temp'].some((k) => d[REMAINING_KEY[k]] !== undefined);
  const busy = Date.now() < sdUploadUntil;
  sdBtn.disabled = !(sdFiles > 0) || measuring || busy;
  sdBtn.textContent = busy ? 'Передаю з SD…'
    : sdFiles > 0 ? `Завантажити дані з SD (${sdFiles})` : 'Завантажити дані з SD';
}

sdBtn.addEventListener('click', async () => {
  if (sdBtn.disabled || !deviceId) return;
  try {
    const r = await fetch('/api/command', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ device_id: deviceId, type: 'sd_upload' }),
    });
    const out = await r.json();
    if (!r.ok) {
      return showNotice(out.via === 'azure'
        ? `Пристрій не виконає команду: ${out.error}`
        : `Сервер не прийняв команду: ${out.error || r.status}`);
    }
    if (!out.online) return showNotice('Пристрій зараз не на зв\'язку.');
    sdUploadUntil = Date.now() + 120 * 1000;
    sdBtn.disabled = true;
    sdBtn.textContent = 'Передаю з SD…';
    showNotice('Пристрій передає файли з SD-картки. Поки триває передача, живих даних не буде.', 15000);
  } catch (e) {
    showNotice(`Не вдалося надіслати команду: ${e.message}`);
  }
});

// Countdowns (warm-up, measurement windows) run on locally between messages:
// outside ECG the device reports only once a minute, and immediately when a
// window opens or closes.
const COUNTDOWN_KEYS = ['lockout_remaining_s', 'ecg_remaining_s', 'vitals_remaining_s', 'temp_remaining_s'];
let lastControls = null;

function tickCountdowns() {
  if (!lastControls) return;
  const elapsed = Math.floor((Date.now() - lastControls.at) / 1000);
  if (elapsed <= 0) return;
  const d = { ...lastControls.d };
  for (const k of COUNTDOWN_KEYS) {
    if (typeof d[k] === 'number') d[k] = Math.max(0, d[k] - elapsed);
  }
  updateControls(d);
}

function handleDeviceMessage(d) {
  if (d.device_id) deviceId = d.device_id;
  showVitals(d);
  lastControls = { d, at: Date.now() };
  updateControls(d);
  updateSdButton(d);
  showLeadOff(d);
  noteData(d.received_at, d.ecg_remaining_s !== undefined);
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { wsOpen = true; updateConnBadge(); loadHistory(); };
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
    if (m.type === 'history') loadHistory(); // a record was saved/deleted, possibly from another tab
    // The server finishes (and saves) a session before announcing it ended,
    // so this is the moment the pending "will be saved" notice comes true.
    if (m.type === 'session' && !m.active && pendingSave && m.device_id === deviceId) {
      pendingSave = false;
      setSaveStatus('Вимірювання збережено.');
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

let noticeTimer = null;
function showNotice(text, ms = 6000) {
  const el = $('notice');
  el.textContent = text;
  el.hidden = false;
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => { el.hidden = true; }, ms);
}

for (const btn of [...measureButtons, $('btn-force')]) {
  btn.addEventListener('click', async () => {
    if (btn.disabled) return;
    if (!deviceId) {
      showNotice('Пристрій ще жодного разу не виходив на зв\'язок - нема кому надіслати команду.');
      return;
    }
    try {
      const r = await fetch('/api/command', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device_id: deviceId, type: btn.dataset.type }),
      });
      const out = await r.json();
      if (!r.ok) {
        showNotice(out.via === 'azure'
          ? `Пристрій не виконає команду: ${out.error}`
          : `Сервер не прийняв команду: ${out.error || r.status}`);
      } else if (!out.online) {
        showNotice('Пристрій зараз не на зв\'язку. Вимір почнеться, якщо він підключиться протягом 30 секунд.');
      }
    } catch (e) {
      showNotice(`Не вдалося надіслати команду: ${e.message}`);
    }
  });
}

// --- saved records -----------------------------------------------------------

const KIND_LABELS = { ecg: 'ЕКГ', vitals: 'пульс/SpO₂', temp: 'температура' };
const TRIGGER_LABELS = { manual: 'вручну', auto: 'авто', sd: 'з SD-картки' };
const RETENTION_LABELS = { 1: '1 день', 7: '1 тиждень', 14: '2 тижні', 30: '1 місяць', 90: '3 місяці', 180: '6 місяців', 365: '1 рік' };
const DAY_MS = 24 * 60 * 60 * 1000;
const VIEW_SECONDS = 10;
const JSON_HEADERS = { 'Content-Type': 'application/json' };

let records = [];
let settings = null;
let viewerRec = null;

function node(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function fmtDateTime(ms) {
  return new Date(ms).toLocaleString('uk-UA', {
    day: '2-digit', month: '2-digit', year: 'numeric', hour: '2-digit', minute: '2-digit',
  });
}

let saveStatusTimer = null;
let pendingSave = false; // "save" pressed mid-measurement, waiting for it to finish

// Success messages clear themselves after a few seconds; errors and the
// "will be saved when it finishes" notice stay until replaced.
function setSaveStatus(text, isError = false, sticky = false) {
  const el = $('save-status');
  el.textContent = text;
  el.className = isError ? 'save-status err' : 'save-status';
  clearTimeout(saveStatusTimer);
  if (!isError && !sticky) saveStatusTimer = setTimeout(() => { el.textContent = ''; }, 6000);
}

function recordValues(r) {
  const parts = [];
  if (typeof r.hr === 'number') parts.push(`пульс ${r.hr} уд/хв`);
  if (typeof r.spo2 === 'number') parts.push(`SpO₂ ${r.spo2}%`);
  if (typeof r.temp === 'number') {
    parts.push(typeof r.temp_body === 'number'
      ? `${r.temp.toFixed(1)} °C (тіло ≈ ${r.temp_body.toFixed(1)} °C)` : `${r.temp.toFixed(1)} °C`);
  }
  if (r.ecg_seconds) parts.push(`ЕКГ ${fmtMMSS(r.ecg_seconds)}`);
  return parts.join(' · ') || 'без даних';
}

function renderRecords() {
  const list = $('records');
  list.replaceChildren();
  $('records-empty').hidden = records.length > 0;
  for (const r of records) {
    const li = node('li');
    const main = node('div', 'rec-main');
    const when = node('div', 'rec-when', fmtDateTime(r.started_at || r.saved_at));
    when.append(node('span', 'badge', TRIGGER_LABELS[r.trigger] || 'авто'));
    if (r.time_unknown) when.append(node('span', 'badge warn', 'час вимірювання невідомий'));
    if (!r.complete) when.append(node('span', 'badge warn', 'перервано'));
    const kinds = (r.kinds || []).map((k) => KIND_LABELS[k] || k).join(', ');
    main.append(when, node('div', 'rec-vals', `${kinds} - ${recordValues(r)}`));

    const actions = node('div', 'rec-actions');
    if (r.ecg_seconds) {
      const view = node('button', 'action-btn', 'Переглянути ЕКГ');
      view.addEventListener('click', () => openViewer(r.id));
      actions.append(view);
    }
    const dl = node('a', 'action-btn', 'Завантажити');
    dl.href = `/api/history/${encodeURIComponent(r.id)}?download=1`;
    const del = node('button', 'action-btn danger', 'Видалити');
    del.addEventListener('click', () => deleteRecord(r));
    actions.append(dl, del);

    li.append(main, actions);
    list.append(li);
  }
}

async function loadHistory() {
  try {
    const out = await (await fetch('/api/history')).json();
    records = out.records;
    settings = out.settings;
    $('autosave').checked = settings.autosave;
    $('retention').value = String(settings.retention_days);
    renderRecords();
  } catch (e) {
    setSaveStatus(`Не вдалося завантажити записи: ${e.message}`, true);
  }
}

async function postSettings(patch) {
  try {
    const r = await fetch('/api/settings', { method: 'POST', headers: JSON_HEADERS, body: JSON.stringify(patch) });
    const out = await r.json();
    if (!r.ok) {
      setSaveStatus(out.error || `помилка ${r.status}`, true);
      return false;
    }
    settings = out.settings;
    setSaveStatus(out.deleted ? `Налаштування збережено, видалено старих записів: ${out.deleted}.` : 'Налаштування збережено.');
    return true;
  } catch (e) {
    setSaveStatus(`Не вдалося зберегти налаштування: ${e.message}`, true);
    return false;
  }
}

$('btn-save').addEventListener('click', async () => {
  if (!deviceId) {
    setSaveStatus('Ще немає пристрою, з якого можна зберегти вимірювання.', true);
    return;
  }
  try {
    const r = await fetch('/api/history/save', {
      method: 'POST', headers: JSON_HEADERS, body: JSON.stringify({ device_id: deviceId }),
    });
    const out = await r.json();
    if (!r.ok) setSaveStatus(out.error || `помилка ${r.status}`, true);
    else if (out.pending) {
      pendingSave = true;
      setSaveStatus('Вимірювання ще триває - його буде збережено повністю, щойно воно завершиться.', false, true);
    }
    else if (out.already) setSaveStatus('Це вимірювання вже збережено.');
    else setSaveStatus('Вимірювання збережено.');
  } catch (e) {
    setSaveStatus(`Не вдалося зберегти: ${e.message}`, true);
  }
});

$('autosave').addEventListener('change', async (e) => {
  if (!(await postSettings({ autosave: e.target.checked }))) e.target.checked = !e.target.checked;
});

$('retention').addEventListener('change', async (e) => {
  const days = Number(e.target.value);
  const prev = settings ? settings.retention_days : days;
  if (days < prev) {
    const cutoff = Date.now() - days * DAY_MS;
    const doomed = records.filter((r) => r.saved_at < cutoff).length;
    if (doomed && !confirm(`Записів, старших за ${RETENTION_LABELS[days]}: ${doomed}. Їх буде видалено одразу. Продовжити?`)) {
      e.target.value = String(prev);
      return;
    }
  }
  if (!(await postSettings({ retention_days: days }))) e.target.value = String(prev);
});

async function deleteRecord(r) {
  if (!confirm(`Видалити запис від ${fmtDateTime(r.started_at || r.saved_at)}? Це не можна скасувати.`)) return;
  const res = await fetch(`/api/history/${encodeURIComponent(r.id)}`, { method: 'DELETE' });
  if (!res.ok) setSaveStatus('Не вдалося видалити запис.', true);
  if (viewerRec && viewerRec.id === r.id) closeViewer();
}

// --- record viewer: the saved ECG, 10 s at a time ----------------------------

async function openViewer(id) {
  try {
    const r = await fetch(`/api/history/${encodeURIComponent(id)}`);
    if (!r.ok) throw new Error(`помилка ${r.status}`);
    viewerRec = await r.json();
  } catch (e) {
    setSaveStatus(`Не вдалося відкрити запис: ${e.message}`, true);
    return;
  }
  const { samples, fs: recFs } = viewerRec.ecg;
  // One fixed scale for the whole recording, so scrolling doesn't make the
  // trace jump in size from one 10 s window to the next.
  const [lo, hi] = percentileRange(samples, SCALE_TRIM);
  const pad = (hi - lo) * SCALE_PAD;
  viewerRec.lo = lo - pad;
  viewerRec.hi = hi + pad;
  viewerRec.totalS = samples.length / recFs;

  $('viewer-title').textContent = `Запис від ${fmtDateTime(viewerRec.started_at || viewerRec.saved_at)}`;
  $('viewer-vitals').textContent = recordValues({ ...viewerRec, ecg_seconds: Math.round(viewerRec.totalS) });
  const pos = $('viewer-pos');
  pos.max = String(Math.max(0, Math.floor(viewerRec.totalS - VIEW_SECONDS)));
  pos.value = '0';
  $('viewer').hidden = false;
  drawViewer();
  $('viewer').scrollIntoView({ behavior: 'smooth', block: 'start' });
}

function closeViewer() {
  viewerRec = null;
  $('viewer').hidden = true;
}

function drawViewer() {
  if (!viewerRec) return;
  const vc = $('viewer-ecg');
  const dpr = window.devicePixelRatio || 1;
  const w = (vc.width = vc.clientWidth * dpr);
  const h = (vc.height = vc.clientHeight * dpr);
  const ctx = vc.getContext('2d');
  const { samples, fs: recFs } = viewerRec.ecg;
  const start = Number($('viewer-pos').value);
  const n = Math.round(VIEW_SECONDS * recFs);
  const slice = samples.slice(Math.round(start * recFs), Math.round(start * recFs) + n);
  const span = viewerRec.hi - viewerRec.lo || 1;

  ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue('--hr');
  ctx.lineWidth = 1.5 * dpr;
  ctx.beginPath();
  slice.forEach((v, i) => {
    const x = (i / (n - 1)) * w;
    const y = h * 0.9 - ((Math.max(viewerRec.lo, Math.min(viewerRec.hi, v)) - viewerRec.lo) / span) * h * 0.8;
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.stroke();
  const end = Math.min(start + VIEW_SECONDS, viewerRec.totalS);
  $('viewer-time').textContent = `${fmtMMSS(start)} - ${fmtMMSS(end)} з ${fmtMMSS(viewerRec.totalS)}`;
}

$('viewer-pos').addEventListener('input', drawViewer);
$('viewer-close').addEventListener('click', closeViewer);
window.addEventListener('resize', drawViewer);

loadHistory();
connect();
setInterval(updateConnBadge, 1000);
setInterval(() => { tickCountdowns(); refreshAges(); }, 1000); // countdowns and "X тому" stay live between messages
(function loop() {
  drawEcg();
  requestAnimationFrame(loop);
})();
