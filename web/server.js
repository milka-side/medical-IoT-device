const http = require('http');
const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const hub = require('./src/hub');
const { parseLine } = require('./src/parser');
const { startMqttIngest } = require('./src/mqtt-ingest');
const { startAzureIngest } = require('./src/azure-ingest');
const commands = require('./src/commands');
const history = require('./src/history');
const azureCommands = require('./src/azure-commands');

hub.addListener(history.observe);
history.setNotifier((payload) => hub.broadcast(payload));

const PORT = Number(process.env.PORT) || 3000;
const HOST = process.env.HOST || '0.0.0.0';
const API_KEY = process.env.API_KEY || '';
const MAX_BODY = 256 * 1024;
// Whole SD file being assembled from the device's pieces (a 10-min ECG is
// ~0.5 MB); anything larger is refused rather than held in memory.
const MAX_SD_FILE = 8 * 1024 * 1024;
const SD_UPLOAD_TTL_MS = 10 * 60 * 1000;
const sdUploads = new Map(); // "device|file" -> { parts: [], size, next, at }
const PUBLIC_DIR = path.join(__dirname, 'public');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
};

function sendJson(res, status, obj) {
  const body = JSON.stringify(obj);
  // Explicit Content-Length instead of Node's default chunked encoding -
  // simpler for the ESP32's HTTP client to consume.
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Access-Control-Allow-Origin': '*',
  });
  res.end(body);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > MAX_BODY) {
        reject(new Error('тіло запиту завелике'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

function serveStatic(req, res) {
  const urlPath = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = path.normalize(path.join(PUBLIC_DIR, urlPath === '/' ? 'index.html' : urlPath));
  if (file !== PUBLIC_DIR && !file.startsWith(PUBLIC_DIR + path.sep)) {
    return sendJson(res, 403, { error: 'forbidden' });
  }
  fs.readFile(file, (err, buf) => {
    if (err) return sendJson(res, 404, { error: 'not found' });
    // No cache headers were being sent at all, which leaves it up to each
    // browser's own heuristics whether to reuse an old copy of app.js -
    // explicit no-cache forces a revalidation every load instead, so a
    // server-side change always shows up on the next refresh.
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file)] || 'application/octet-stream',
      'Cache-Control': 'no-cache',
    });
    res.end(buf);
  });
}

function summarize(results) {
  const accepted = results.filter((r) => r.ok).length;
  const errors = results.filter((r) => !r.ok).map((r) => r.error);
  const warnings = results.filter((r) => r.ok).flatMap((r) => r.warnings || []);
  const out = { accepted, rejected: errors.length, errors: errors.slice(0, 10) };
  if (warnings.length) out.warnings = warnings.slice(0, 10);
  return out;
}

const server = http.createServer(async (req, res) => {
  const reqUrl = new URL(req.url, 'http://x');
  const { pathname } = reqUrl;

  if (req.method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, DELETE, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type, X-API-Key',
    });
    return res.end();
  }

  if (pathname.startsWith('/api/') && req.method === 'POST') {
    // X-API-Key guards what the *device* sends. The dashboard's own
    // endpoints (buttons, saving, settings) can't carry a secret from a
    // browser page.
    const deviceEndpoint = pathname === '/api/data' || pathname === '/api/line' || pathname === '/api/sd-upload';
    if (deviceEndpoint && API_KEY && req.headers['x-api-key'] !== API_KEY) {
      return sendJson(res, 401, { error: 'невірний або відсутній X-API-Key' });
    }
    let body;
    try { body = await readBody(req); } catch (e) { return sendJson(res, 413, { error: e.message }); }

    if (pathname === '/api/data') {
      let parsed;
      try { parsed = JSON.parse(body); } catch { return sendJson(res, 400, { error: 'некоректний JSON' }); }
      const items = Array.isArray(parsed) ? parsed : [parsed];
      const out = summarize(items.map((it) => hub.ingest(it, 'http')));
      // Queued button presses ride back on the device's own telemetry post,
      // so it needs no separate polling request (one radio wake-up a second
      // instead of two). Included even if this batch was rejected - the
      // device is clearly online and listening.
      const ids = [...new Set(items.map((it) => it && it.device_id).filter(Boolean).map((id) => String(id).slice(0, 64)))];
      out.commands = [...new Set(ids.flatMap((id) => commands.takeCommands(id)))];
      return sendJson(res, out.accepted ? 200 : 400, out);
    }

    if (pathname === '/api/line') {
      const results = body.split(/\r?\n/).filter((l) => l.trim()).map((l) => {
        const obj = parseLine(l);
        return obj ? hub.ingest(obj, 'line') : { ok: false, error: `не вдалося розібрати рядок: ${l.slice(0, 80)}` };
      });
      const out = summarize(results);
      return sendJson(res, out.accepted ? 200 : 400, out);
    }

    if (pathname === '/api/sd-upload') {
      // One piece of a measurement file from the device's SD card. Pieces
      // arrive in order; on the last one the file becomes history records
      // and only then does the device delete it from the card.
      const q = reqUrl.searchParams;
      const deviceId = (q.get('device_id') || '').slice(0, 64);
      const file = (q.get('file') || '').slice(0, 64);
      const part = Number(q.get('part'));
      if (!deviceId || !file || !Number.isInteger(part) || part < 0) {
        return sendJson(res, 400, { error: 'потрібні device_id, file, part' });
      }
      const now = Date.now();
      for (const [k, u] of sdUploads) if (now - u.at > SD_UPLOAD_TTL_MS) sdUploads.delete(k);
      const key = `${deviceId}|${file}`;
      if (part === 0) sdUploads.set(key, { parts: [], size: 0, next: 0, at: now });
      const up = sdUploads.get(key);
      if (!up || up.next !== part) {
        sdUploads.delete(key);
        return sendJson(res, 409, { error: `очікувалась частина ${up ? up.next : 0}, прийшла ${part}` });
      }
      up.parts.push(body);
      up.size += body.length;
      up.next++;
      up.at = now;
      if (up.size > MAX_SD_FILE) {
        sdUploads.delete(key);
        return sendJson(res, 413, { error: 'файл із SD-картки завеликий' });
      }
      if (q.get('last') !== '1') return sendJson(res, 200, { ok: true });
      sdUploads.delete(key);
      const ids = history.importSdFile(deviceId, up.parts.join(''), {
        boot: Number(q.get('boot')), uptime: Number(q.get('uptime')), now,
      });
      console.log(`[sd] ${deviceId} ${file}: ${up.size} байт -> записів: ${ids.length}`);
      return sendJson(res, 200, { ok: true, records: ids.length });
    }

    if (pathname === '/api/command') {
      // A dashboard button. First choice: an Azure IoT Hub direct method -
      // the device answers right away whether it will do it. If Azure isn't
      // configured or the device isn't connected to the hub, the command is
      // queued for the device's next HTTP telemetry post instead.
      let parsed;
      try { parsed = JSON.parse(body); } catch { return sendJson(res, 400, { error: 'некоректний JSON' }); }
      const deviceId = String(parsed.device_id ?? '').slice(0, 64);
      if (!deviceId) return sendJson(res, 400, { error: 'відсутній device_id' });
      if (!commands.VALID_TYPES.has(parsed.type)) {
        return sendJson(res, 400, { ok: false, error: `невідома команда: ${parsed.type}` });
      }
      const az = await azureCommands.sendCommand(parsed.type);
      if (az && az.delivered) {
        if (az.status === 200) return sendJson(res, 200, { ok: true, online: true, via: 'azure' });
        return sendJson(res, 409, { ok: false, via: 'azure', error: az.reason || `код ${az.status}` });
      }
      const result = commands.setCommand(deviceId, parsed.type);
      result.via = 'http';
      if (az) console.log(`[azure] direct method не вдався (${az.error}) - команда піде через HTTP`);
      return sendJson(res, result.ok ? 200 : 400, result);
    }

    if (pathname === '/api/history/save') {
      let parsed;
      try { parsed = JSON.parse(body || '{}'); } catch { return sendJson(res, 400, { error: 'некоректний JSON' }); }
      const deviceId = String(parsed.device_id ?? '').slice(0, 64);
      if (!deviceId) return sendJson(res, 400, { error: 'відсутній device_id' });
      const result = history.manualSave(deviceId);
      return sendJson(res, result.ok ? 200 : 409, result);
    }

    if (pathname === '/api/settings') {
      let parsed;
      try { parsed = JSON.parse(body || '{}'); } catch { return sendJson(res, 400, { error: 'некоректний JSON' }); }
      const result = history.updateSettings(parsed);
      return sendJson(res, result.ok ? 200 : 400, result);
    }
  }

  if (req.method === 'GET' && pathname === '/api/history') {
    return sendJson(res, 200, { records: history.listRecords(), settings: history.getSettings() });
  }

  const recordMatch = pathname.match(/^\/api\/history\/([^/]+)$/);
  if (recordMatch && req.method === 'GET') {
    const file = history.recordPath(recordMatch[1]);
    if (!file) return sendJson(res, 404, { error: 'запис не знайдено' });
    const headers = {
      'Content-Type': 'application/json; charset=utf-8',
      'Content-Length': fs.statSync(file).size,
    };
    if (reqUrl.searchParams.has('download')) {
      headers['Content-Disposition'] = `attachment; filename="ecg-record-${recordMatch[1]}.json"`;
    }
    res.writeHead(200, headers);
    return fs.createReadStream(file).pipe(res);
  }
  if (recordMatch && req.method === 'DELETE') {
    return history.deleteRecord(recordMatch[1])
      ? sendJson(res, 200, { ok: true })
      : sendJson(res, 404, { error: 'запис не знайдено' });
  }

  if (req.method === 'GET' && pathname === '/api/settings') return sendJson(res, 200, history.getSettings());
  if (req.method === 'GET' && pathname === '/api/session') {
    const deviceId = (reqUrl.searchParams.get('device_id') || '').slice(0, 64);
    return sendJson(res, 200, history.sessionState(deviceId));
  }

  if (req.method === 'GET' && pathname === '/api/latest') return sendJson(res, 200, hub.getLatest());
  if (req.method === 'GET' && pathname === '/api/health') return sendJson(res, 200, { ok: true, ...hub.getStats() });
  if (req.method === 'GET') return serveStatic(req, res);

  sendJson(res, 404, { error: 'not found' });
});

const wss = new WebSocketServer({ server, path: '/ws' });
wss.on('connection', (ws) => hub.addClient(ws));

startMqttIngest(hub);
startAzureIngest(hub);
azureCommands.startAzureCommands();

server.listen(PORT, HOST, () => {
  console.log(`Панель: http://localhost:${PORT}`);
  console.log(`Прийом даних: POST http://<IP цього комп'ютера>:${PORT}/api/data`);
  if (API_KEY) console.log('Увімкнено перевірку API-Key');
  if (process.env.INGEST_ONLY) console.log(`Показуються лише дані з: ${process.env.INGEST_ONLY}`);
});
