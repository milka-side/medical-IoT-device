const DEFAULT_FS = 250;

// Значення поза діапазоном це помилка датчика
const RANGES = {
  hr: [20, 300],
  spo2: [50, 100],
  temp: [25, 45],
  battery: [0, 100],
};

function num(v) {
  if (v === null || v === undefined || v === '') return undefined;
  const n = Number(v);
  return Number.isFinite(n) ? n : undefined;
}

function inRange(key, v) {
  if (v === undefined) return undefined;
  const r = RANGES[key];
  return !r || (v >= r[0] && v <= r[1]) ? v : undefined;
}

function normalize(input, receivedAt = Date.now()) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) {
    return { ok: false, error: 'очікується JSON-об\'єкт' };
  }

  const msg = {
    device_id: String(input.device_id ?? input.id ?? 'unknown').slice(0, 64),
    ts: num(input.ts) ?? receivedAt,
    received_at: receivedAt,
  };

  const dropped = [];
  const pick = (key, raw) => {
    const v = num(raw);
    const ok = inRange(key, v);
    if (v !== undefined && ok === undefined) dropped.push(`${key}=${v} поза діапазоном ${RANGES[key].join('..')}`);
    return ok;
  };
  const hr = pick('hr', input.hr ?? input.bpm);
  const spo2 = pick('spo2', input.spo2);
  const temp = pick('temp', input.temp ?? input.temperature);
  const battery = pick('battery', input.battery);
  if (hr !== undefined) msg.hr = hr;
  if (spo2 !== undefined) msg.spo2 = spo2;
  if (temp !== undefined) msg.temp = temp;
  if (battery !== undefined) msg.battery = battery;

  if (input.ecg !== undefined) {
    const arr = Array.isArray(input.ecg) ? input.ecg : [input.ecg];
    const samples = arr.map(num).filter((v) => v !== undefined);
    if (samples.length > 0) {
      msg.ecg = samples.slice(0, 5000);
      msg.fs = num(input.fs) ?? DEFAULT_FS;
    }
  }

  if (msg.hr === undefined && msg.spo2 === undefined && msg.temp === undefined && !msg.ecg) {
    const why = dropped.length ? dropped.join(', ') : 'немає жодного поля з даними (ecg, hr, spo2, temp)';
    return { ok: false, error: why };
  }
  return { ok: true, msg };
}

function parseLine(line) {
  const trimmed = String(line).trim();
  if (!trimmed) return null;
  if (trimmed.startsWith('{')) {
    try { return JSON.parse(trimmed); } catch { return null; }
  }
  const obj = {};
  for (const part of trimmed.split(/[;\s]+/)) {
    const m = part.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*[:=]\s*(.+)$/);
    if (!m) continue;
    const key = m[1].toLowerCase();
    obj[key] = key === 'ecg' ? m[2].split(',') : m[2];
  }
  return Object.keys(obj).length ? obj : null;
}

module.exports = { normalize, parseLine, DEFAULT_FS };
