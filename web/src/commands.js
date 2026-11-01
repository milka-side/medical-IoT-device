// One-shot command queue: the dashboard POSTs a command for a device_id,
// the device polls GET /api/command and the act of polling consumes it.
const VALID_TYPES = new Set(['ecg', 'vitals', 'temp', 'all']);

// A press the device never picked up (it was offline / rebooting) must not
// fire hours later out of nowhere - a live device polls every ~1 s.
const COMMAND_TTL_MS = 30 * 1000;

const pending = new Map(); // device_id -> { type, createdAt }

function setCommand(deviceId, type) {
  if (!VALID_TYPES.has(type)) {
    return { ok: false, error: `невідомий тип вимірювання: ${type}` };
  }
  pending.set(deviceId, { type, createdAt: Date.now() });
  return { ok: true };
}

function takeCommand(deviceId) {
  const cmd = pending.get(deviceId);
  if (!cmd) return null;
  pending.delete(deviceId);
  return Date.now() - cmd.createdAt <= COMMAND_TTL_MS ? cmd.type : null;
}

module.exports = { setCommand, takeCommand, VALID_TYPES };
