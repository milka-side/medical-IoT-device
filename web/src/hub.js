const { normalize } = require('./parser');

const latest = new Map();
const clients = new Set();

const stats = { received: 0, rejected: 0, started_at: Date.now() };

function broadcast(payload) {
  const text = JSON.stringify(payload);
  for (const ws of clients) {
    if (ws.readyState === 1) ws.send(text);
  }
}

// Drop repeats by boot+seq (Event Hub can redeliver).
const DEDUPE_TTL_MS = 120000;
const seen = new Map();

function isDuplicate(raw) {
  if (!raw || typeof raw !== 'object' || raw.boot === undefined || raw.seq === undefined) return false;
  const key = `${raw.device_id}|${raw.boot}|${raw.seq}`;
  const now = Date.now();
  if (seen.size > 5000) {
    for (const [k, t] of seen) if (now - t > DEDUPE_TTL_MS) seen.delete(k);
  }
  if (seen.has(key) && now - seen.get(key) <= DEDUPE_TTL_MS) return true;
  seen.set(key, now);
  return false;
}

function ingest(raw, source) {
  if (isDuplicate(raw)) {
    stats.duplicates = (stats.duplicates || 0) + 1;
    return { ok: true, duplicate: true };
  }
  const res = normalize(raw);
  if (!res.ok) {
    stats.rejected++;
    return res;
  }
  const msg = { ...res.msg, source };
  stats.received++;
  if (res.warnings && res.warnings.length) {
    console.warn(`[hub] ${msg.device_id}: ${res.warnings.join('; ')}`);
  }

  const prev = latest.get(msg.device_id) || {};
  const { ecg, fs, ...vitals } = msg;
  const merged = { ...prev, ...vitals };
  // Countdowns only exist while running. Drop them when a message comes
  // without them, or a reconnecting browser sees a stale timer.
  for (const key of ['lockout_remaining_s', 'ecg_remaining_s', 'vitals_remaining_s', 'temp_remaining_s', 'lod', 'adc_sps']) {
    if (!(key in vitals)) delete merged[key];
  }
  latest.set(msg.device_id, merged);

  broadcast({ type: 'data', data: msg });
  for (const fn of listeners) {
    try { fn(msg); } catch (e) { console.error('[hub] listener failed:', e.message); }
  }
  return res;
}

// Extra consumers of every message (history).
const listeners = [];
function addListener(fn) { listeners.push(fn); }

function addClient(ws) {
  clients.add(ws);
  ws.send(JSON.stringify({ type: 'snapshot', devices: Object.fromEntries(latest) }));
  ws.on('close', () => clients.delete(ws));
}

function getLatest() {
  return Object.fromEntries(latest);
}

function getStats() {
  return { ...stats, clients: clients.size, devices: latest.size };
}

module.exports = { ingest, addClient, getLatest, getStats, addListener, broadcast };
