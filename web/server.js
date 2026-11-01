const http = require('http');
const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const hub = require('./src/hub');
const { parseLine } = require('./src/parser');
const { startMqttIngest } = require('./src/mqtt-ingest');
const { startAzureIngest } = require('./src/azure-ingest');
const commands = require('./src/commands');

const PORT = Number(process.env.PORT) || 3000;
const HOST = process.env.HOST || '0.0.0.0';
const API_KEY = process.env.API_KEY || '';
const MAX_BODY = 256 * 1024;
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
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type, X-API-Key',
    });
    return res.end();
  }

  if (pathname.startsWith('/api/') && req.method === 'POST') {
    if (API_KEY && req.headers['x-api-key'] !== API_KEY) {
      return sendJson(res, 401, { error: 'невірний або відсутній X-API-Key' });
    }
    let body;
    try { body = await readBody(req); } catch (e) { return sendJson(res, 413, { error: e.message }); }

    if (pathname === '/api/data') {
      let parsed;
      try { parsed = JSON.parse(body); } catch { return sendJson(res, 400, { error: 'некоректний JSON' }); }
      const items = Array.isArray(parsed) ? parsed : [parsed];
      const out = summarize(items.map((it) => hub.ingest(it, 'http')));
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

    if (pathname === '/api/command') {
      // Дашборд ставить команду тут; пристрій забирає її через GET нижче.
      let parsed;
      try { parsed = JSON.parse(body); } catch { return sendJson(res, 400, { error: 'некоректний JSON' }); }
      const deviceId = String(parsed.device_id ?? '').slice(0, 64);
      if (!deviceId) return sendJson(res, 400, { error: 'відсутній device_id' });
      const result = commands.setCommand(deviceId, parsed.type);
      return sendJson(res, result.ok ? 200 : 400, result);
    }
  }

  if (req.method === 'GET' && pathname === '/api/command') {
    // Пристрій опитує це раз на секунду, поки простоює; одне звернення
    // одразу забирає команду (one-shot), як і POST /api/data захищено
    // X-API-Key, коли він заданий на сервері.
    if (API_KEY && req.headers['x-api-key'] !== API_KEY) {
      return sendJson(res, 401, { error: 'невірний або відсутній X-API-Key' });
    }
    const deviceId = (reqUrl.searchParams.get('device_id') || '').slice(0, 64);
    if (!deviceId) return sendJson(res, 400, { error: 'відсутній device_id' });
    return sendJson(res, 200, { type: commands.takeCommand(deviceId) });
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

server.listen(PORT, HOST, () => {
  console.log(`Панель: http://localhost:${PORT}`);
  console.log(`Прийом даних: POST http://<IP цього комп'ютера>:${PORT}/api/data`);
  if (API_KEY) console.log('Увімкнено перевірку API-Key');
});
