// Command queue: the dashboard POSTs "measure X" (or "upload the SD card")
// for a device_id; the device picks everything queued since last time out
// of the response to its once-a-second telemetry POST /api/data. Taking the
// commands consumes them.
// all_force = "Виміряти все одно": measure everything even during the
// device's power-on sensor warm-up.
const VALID_TYPES = new Set(['ecg', 'vitals', 'temp', 'all', 'all_force', 'sd_upload']);

// A press the device never picked up (it was offline / rebooting) must not
// fire hours later out of nowhere - a live device polls every ~1 s.
const COMMAND_TTL_MS = 30 * 1000;

// No poll for this long = the device is treated as offline.
const ONLINE_WINDOW_MS = 5 * 1000;

// device_id -> Map<type, createdAt>. Several different buttons pressed
// between two polls must all reach the device; a single slot per device
// meant "ECG" then "temperature" within a second silently lost the ECG.
const pending = new Map();
const lastPollAt = new Map();

function setCommand(deviceId, type) {
  if (!VALID_TYPES.has(type)) {
    return { ok: false, error: `невідома команда: ${type}` };
  }
  if (!pending.has(deviceId)) pending.set(deviceId, new Map());
  pending.get(deviceId).set(type, Date.now());
  return { ok: true, online: isOnline(deviceId) };
}

// Returns the still-fresh pending types (possibly empty) and clears the queue.
function takeCommands(deviceId) {
  lastPollAt.set(deviceId, Date.now());
  const queued = pending.get(deviceId);
  if (!queued) return [];
  pending.delete(deviceId);
  const now = Date.now();
  return [...queued].filter(([, at]) => now - at <= COMMAND_TTL_MS).map(([type]) => type);
}

function isOnline(deviceId) {
  const at = lastPollAt.get(deviceId);
  return at !== undefined && Date.now() - at <= ONLINE_WINDOW_MS;
}

module.exports = { setCommand, takeCommands, isOnline, VALID_TYPES };
