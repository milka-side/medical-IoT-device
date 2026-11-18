// Saved measurement records: one record = one measurement session (from the
// first window opening until every window - ECG / vitals / temp - has
// closed), with the full ECG trace and the session's averaged vitals.
//
// Stored as files under DATA_DIR (web/data by default, git-ignored - this is
// patient data): records/<id>.json holds the full record, records/<id>.meta.json
// a small summary so listing never has to read megabytes of ECG samples.
// The id starts with the save time in ms, so retention cleanup can work
// from file names alone.
const fs = require('fs');
const { normalize } = require('./parser');
const path = require('path');
const crypto = require('crypto');

const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, '..', 'data');
const RECORDS_DIR = path.join(DATA_DIR, 'records');
const SETTINGS_FILE = path.join(DATA_DIR, 'settings.json');

const DAY_MS = 24 * 60 * 60 * 1000;
const RETENTION_OPTIONS = [1, 7, 14, 30, 90, 180, 365]; // days
const DEFAULT_SETTINGS = { retention_days: 30, autosave: true };

// A session whose device went silent this long is closed as interrupted.
const SESSION_TIMEOUT_MS = 30 * 1000;
// Repeated button presses can extend an ECG window indefinitely; cap what a
// single record keeps in memory / on disk (30 min at 200 Hz ~ 2.5 MB JSON).
const MAX_ECG_SECONDS = 30 * 60;

const ID_RE = /^\d{13}-[0-9a-f]{8}$/;

fs.mkdirSync(RECORDS_DIR, { recursive: true });

let notify = () => {};
let settings = loadSettings();

const sessions = new Map();     // device_id -> in-progress session
const lastFinished = new Map(); // device_id -> most recent finished session

function loadSettings() {
  try {
    const s = JSON.parse(fs.readFileSync(SETTINGS_FILE, 'utf8'));
    return {
      retention_days: RETENTION_OPTIONS.includes(s.retention_days) ? s.retention_days : DEFAULT_SETTINGS.retention_days,
      autosave: typeof s.autosave === 'boolean' ? s.autosave : DEFAULT_SETTINGS.autosave,
    };
  } catch {
    return { ...DEFAULT_SETTINGS };
  }
}

// Write to a temp file then rename, so a crash mid-write never leaves a
// half-written (unparseable) record or settings file behind.
function writeAtomic(file, text) {
  const tmp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, text);
  fs.renameSync(tmp, file);
}

function getSettings() {
  return { ...settings, retention_options: RETENTION_OPTIONS };
}

function updateSettings(patch) {
  const next = { ...settings };
  if (patch.retention_days !== undefined) {
    const d = Number(patch.retention_days);
    if (!RETENTION_OPTIONS.includes(d)) return { ok: false, error: `недопустимий термін зберігання: ${patch.retention_days}` };
    next.retention_days = d;
  }
  if (patch.autosave !== undefined) {
    if (typeof patch.autosave !== 'boolean') return { ok: false, error: 'autosave має бути true/false' };
    next.autosave = patch.autosave;
  }
  settings = next;
  writeAtomic(SETTINGS_FILE, JSON.stringify(settings, null, 2));
  const deleted = cleanup();
  notify({ type: 'history' });
  return { ok: true, settings: getSettings(), deleted };
}

// --- records on disk -------------------------------------------------------

function recordFiles(id) {
  return { full: path.join(RECORDS_DIR, `${id}.json`), meta: path.join(RECORDS_DIR, `${id}.meta.json`) };
}

function summarize(rec) {
  const { ecg, ...meta } = rec;
  meta.ecg_seconds = ecg ? Math.round(ecg.samples.length / ecg.fs) : 0;
  return meta;
}

function saveSession(sess, trigger) {
  const id = `${Date.now()}-${crypto.randomBytes(4).toString('hex')}`;
  const rec = {
    id,
    device_id: sess.device_id,
    trigger,
    complete: sess.complete !== false,
    started_at: sess.started_at,
    ended_at: sess.ended_at || Date.now(),
    saved_at: Date.now(),
    kinds: [...sess.kinds],
    hr: sess.hr,
    spo2: sess.spo2,
    temp: sess.temp,           // measured fingertip temperature
    temp_body: sess.temp_body, // estimated body temperature
    ecg: sess.ecg.length ? { fs: sess.fs, samples: sess.ecg } : null,
  };
  // Records restored from the device's SD card (measured while offline).
  if (sess.source) rec.source = sess.source;
  if (sess.time_unknown) rec.time_unknown = true;
  const files = recordFiles(id);
  writeAtomic(files.full, JSON.stringify(rec));
  writeAtomic(files.meta, JSON.stringify(summarize(rec)));
  sess.saved_id = id;
  cleanup();
  notify({ type: 'history' });
  return id;
}

function listRecords() {
  const out = [];
  for (const name of fs.readdirSync(RECORDS_DIR)) {
    if (!name.endsWith('.meta.json')) continue;
    try {
      out.push(JSON.parse(fs.readFileSync(path.join(RECORDS_DIR, name), 'utf8')));
    } catch {
      // unreadable summary - skip it rather than fail the whole listing
    }
  }
  return out.sort((a, b) => b.saved_at - a.saved_at);
}

function recordPath(id) {
  if (!ID_RE.test(id)) return null;
  const file = recordFiles(id).full;
  return fs.existsSync(file) ? file : null;
}

function deleteRecord(id) {
  if (!ID_RE.test(id)) return false;
  const files = recordFiles(id);
  let removed = false;
  for (const f of [files.full, files.meta]) {
    try { fs.unlinkSync(f); removed = true; } catch { /* already gone */ }
  }
  if (removed) notify({ type: 'history' });
  return removed;
}

// Deletes records older than the retention period; returns how many.
function cleanup() {
  const cutoff = Date.now() - settings.retention_days * DAY_MS;
  let deleted = 0;
  for (const name of fs.readdirSync(RECORDS_DIR)) {
    const m = name.match(/^(\d{13})-[0-9a-f]{8}\.(meta\.)?json$/);
    if (m && Number(m[1]) < cutoff) {
      try { fs.unlinkSync(path.join(RECORDS_DIR, name)); } catch { /* ignore */ }
      if (!m[2]) deleted++;
    }
  }
  return deleted;
}

// --- session tracking from live telemetry -----------------------------------

function finishSession(sess, complete) {
  sessions.delete(sess.device_id);
  sess.complete = complete;
  sess.ended_at = Date.now();
  lastFinished.set(sess.device_id, sess);
  if (settings.autosave || sess.save_requested) {
    saveSession(sess, sess.save_requested ? 'manual' : 'auto');
  }
  notify({ type: 'session', device_id: sess.device_id, active: false });
}

// Called for every accepted telemetry message (see hub.addListener).
function observe(msg) {
  const active = msg.ecg_remaining_s !== undefined
    || msg.vitals_remaining_s !== undefined
    || msg.temp_remaining_s !== undefined;
  let sess = sessions.get(msg.device_id);

  if (!sess && active) {
    sess = {
      device_id: msg.device_id,
      started_at: Date.now(),
      kinds: new Set(),
      ecg: [],
      fs: 0,
      save_requested: false,
    };
    sessions.set(msg.device_id, sess);
    notify({ type: 'session', device_id: msg.device_id, active: true });
  }
  if (!sess) return;

  sess.last_msg_at = Date.now();
  // The last ECG batch of a window arrives in the same message that no
  // longer carries ecg_remaining_s, so append before deciding to finish.
  if (msg.ecg) {
    sess.fs = msg.fs || sess.fs;
    if (sess.ecg.length < MAX_ECG_SECONDS * sess.fs) sess.ecg.push(...msg.ecg);
  }
  if (msg.ecg_remaining_s !== undefined) sess.kinds.add('ecg');
  if (msg.vitals_remaining_s !== undefined) sess.kinds.add('vitals');
  if (msg.temp_remaining_s !== undefined) sess.kinds.add('temp');
  // hr/spo2/temp are already running averages over the window on the
  // device, so the latest value is the window's final average.
  if (msg.hr !== undefined) sess.hr = msg.hr;
  if (msg.spo2 !== undefined) sess.spo2 = msg.spo2;
  if (msg.temp !== undefined) sess.temp = msg.temp;
  if (msg.temp_body !== undefined) sess.temp_body = msg.temp_body;

  if (!active) finishSession(sess, true);
}

setInterval(() => {
  const now = Date.now();
  for (const sess of sessions.values()) {
    if (now - sess.last_msg_at > SESSION_TIMEOUT_MS) finishSession(sess, false);
  }
}, 5000).unref();

setInterval(() => { if (cleanup()) notify({ type: 'history' }); }, 60 * 60 * 1000).unref();
cleanup();

// The "Зберегти" button. During a measurement it just marks the session so
// the complete record is saved when it ends (saving a half-finished copy
// now would leave a partial duplicate next to the auto-saved full one);
// otherwise it saves the most recent finished session.
function manualSave(deviceId) {
  const active = sessions.get(deviceId);
  if (active) {
    active.save_requested = true;
    return { ok: true, pending: true };
  }
  const last = lastFinished.get(deviceId);
  if (!last) return { ok: false, error: 'ще немає жодного вимірювання, яке можна зберегти' };
  if (last.saved_id && recordPath(last.saved_id)) return { ok: true, already: true, id: last.saved_id };
  return { ok: true, id: saveSession(last, 'manual') };
}

function sessionState(deviceId) {
  const active = sessions.get(deviceId);
  return { active: !!active, save_requested: !!active?.save_requested };
}

function setNotifier(fn) { notify = fn; }

// --- measurements the device saved on its SD card while offline -----------
//
// A file is the device's telemetry lines (same JSON as POST /api/data), one
// per second, grouped into measurements by header lines
// {"device_id","boot","uptime_s"[,"epoch"]} that start each one. Each group
// becomes one saved record. Time: the header's epoch if the device clock was
// synced; else, if the device hasn't rebooted since (same boot id), work back
// from its current uptime; else it's unknown and the upload time is used.
function importSdFile(deviceId, text, { boot, uptime, now = Date.now() } = {}) {
  const groups = [];
  let cur = null;
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue;
    let obj;
    try { obj = JSON.parse(line); } catch { continue; } // torn last line after a power cut
    const isHeader = obj.uptime_s !== undefined && obj.seq === undefined && obj.status === undefined;
    if (isHeader) {
      cur = { header: obj, lines: [] };
      groups.push(cur);
      continue;
    }
    if (!cur) {
      cur = { header: null, lines: [] };
      groups.push(cur);
    }
    cur.lines.push(obj);
  }

  const ids = [];
  for (const g of groups) {
    const sess = {
      device_id: deviceId, kinds: new Set(), ecg: [], fs: 0, complete: true, source: 'sd',
    };
    let n = 0;
    for (const raw of g.lines) {
      const r = normalize({ ...raw, device_id: deviceId });
      if (!r.ok) continue;
      const m = r.msg;
      n++;
      if (m.ecg) {
        sess.fs = m.fs || sess.fs;
        if (sess.ecg.length < MAX_ECG_SECONDS * sess.fs) sess.ecg.push(...m.ecg);
      }
      if (m.ecg_remaining_s !== undefined || m.ecg) sess.kinds.add('ecg');
      if (m.vitals_remaining_s !== undefined || m.hr !== undefined) sess.kinds.add('vitals');
      if (m.temp_remaining_s !== undefined || m.temp !== undefined) sess.kinds.add('temp');
      if (m.hr !== undefined) sess.hr = m.hr;
      if (m.spo2 !== undefined) sess.spo2 = m.spo2;
      if (m.temp !== undefined) sess.temp = m.temp;
      if (m.temp_body !== undefined) sess.temp_body = m.temp_body;
    }
    if (!n) continue;

    const h = g.header || {};
    let startedAt;
    if (Number.isFinite(h.epoch)) {
      startedAt = h.epoch * 1000;
    } else if (h.boot !== undefined && Number(h.boot) === Number(boot) && Number.isFinite(uptime) && Number.isFinite(h.uptime_s)) {
      startedAt = now - (uptime - h.uptime_s) * 1000;
    } else {
      startedAt = now;
      sess.time_unknown = true;
    }
    const seconds = Math.max(n, sess.fs ? sess.ecg.length / sess.fs : 0);
    sess.started_at = Math.round(startedAt);
    sess.ended_at = Math.round(startedAt + seconds * 1000);
    ids.push(saveSession(sess, 'sd'));
  }
  return ids;
}

module.exports = {
  observe, manualSave, sessionState, listRecords, recordPath, deleteRecord, importSdFile,
  getSettings, updateSettings, setNotifier, RETENTION_OPTIONS,
};
