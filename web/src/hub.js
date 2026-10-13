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

function ingest(raw, source) {
  const res = normalize(raw);
  if (!res.ok) {
    stats.rejected++;
    return res;
  }
  const msg = { ...res.msg, source };
  stats.received++;

  const prev = latest.get(msg.device_id) || {};
  const { ecg, fs, ...vitals } = msg;
  latest.set(msg.device_id, { ...prev, ...vitals });

  broadcast({ type: 'data', data: msg });
  return res;
}

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

module.exports = { ingest, addClient, getLatest, getStats };
