#!/usr/bin/env node
const crypto = require('crypto');
const mqtt = require('mqtt');

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
    return acc;
  }, [])
);

const CONNECTION_STRING = args['connection-string'] || process.env.AZURE_DEVICE_CONNECTION_STRING;
if (!CONNECTION_STRING) {
  console.error('Потрібен рядок підключення пристрою: --connection-string "HostName=...;DeviceId=...;SharedAccessKey=..."');
  console.error('або змінна середовища AZURE_DEVICE_CONNECTION_STRING');
  process.exit(1);
}

function parseConnectionString(cs) {
  const out = {};
  for (const part of cs.split(';')) {
    const i = part.indexOf('=');
    if (i === -1) continue;
    out[part.slice(0, i)] = part.slice(i + 1);
  }
  if (!out.HostName || !out.DeviceId || !out.SharedAccessKey) {
    throw new Error('невірний формат рядка підключення (очікується HostName=...;DeviceId=...;SharedAccessKey=...)');
  }
  return out;
}

function generateSasToken(resourceUri, signingKey, expiresInSeconds = 3600) {
  const encodedUri = encodeURIComponent(resourceUri);
  const expiry = Math.floor(Date.now() / 1000) + expiresInSeconds;
  const toSign = `${encodedUri}\n${expiry}`;
  const signature = crypto.createHmac('sha256', Buffer.from(signingKey, 'base64')).update(toSign, 'utf8').digest('base64');
  return `SharedAccessSignature sr=${encodedUri}&sig=${encodeURIComponent(signature)}&se=${expiry}`;
}

const { HostName, DeviceId, SharedAccessKey } = parseConnectionString(CONNECTION_STRING);

const FS = 250;
const BATCH = 50;
const BATCH_MS = 200;
const VITALS_EVERY = 5;
const TOKEN_TTL_S = 3600;

const VITALS = { hr: 72, spo2: 98, temp: 36.6, battery: 87 };

const ECG_BEAT = [
  2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
  2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2049, 2049, 2050, 2051, 2053, 2056, 2060,
  2065, 2072, 2081, 2091, 2102, 2113, 2124, 2133, 2140, 2143, 2144, 2141, 2134, 2126, 2115, 2104,
  2093, 2083, 2074, 2067, 2061, 2057, 2054, 2051, 2050, 2049, 2049, 2048, 2048, 2048, 2048, 2048,
  2048, 2048, 2048, 2048, 2048, 2047, 2043, 2034, 2018, 1999, 1989, 2008, 2076, 2205, 2388, 2594,
  2767, 2844, 2793, 2631, 2411, 2195, 2024, 1925, 1898, 1925, 1971, 2010, 2033, 2044, 2047, 2048,
  2048, 2048, 2048, 2048, 2048, 2048, 2048, 2049, 2049, 2049, 2050, 2050, 2051, 2052, 2054, 2056,
  2058, 2061, 2065, 2070, 2075, 2082, 2089, 2098, 2108, 2119, 2131, 2144, 2157, 2172, 2186, 2201,
  2215, 2228, 2240, 2251, 2259, 2266, 2270, 2272, 2271, 2268, 2262, 2254, 2244, 2233, 2220, 2206,
  2192, 2177, 2163, 2149, 2136, 2124, 2112, 2102, 2093, 2085, 2078, 2072, 2067, 2063, 2060, 2057,
  2055, 2053, 2052, 2051, 2050, 2049, 2049, 2049, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
  2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
  2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
];

let pos = 0;
let batch = 0;

function makeMessage() {
  const ecg = [];
  for (let i = 0; i < BATCH; i++) {
    ecg.push(ECG_BEAT[pos]);
    pos = (pos + 1) % ECG_BEAT.length;
  }
  const msg = { device_id: DeviceId, fs: FS, ecg };
  if (batch++ % VITALS_EVERY === 0) Object.assign(msg, VITALS);
  return msg;
}

const topic = `devices/${DeviceId}/messages/events/`;
let client;

function connect() {
  const resourceUri = `${HostName}/devices/${DeviceId}`;
  const sasToken = generateSasToken(resourceUri, SharedAccessKey, TOKEN_TTL_S);

  client = mqtt.connect(`mqtts://${HostName}:8883`, {
    clientId: DeviceId,
    username: `${HostName}/${DeviceId}/?api-version=2021-04-12`,
    password: sasToken,
    protocolVersion: 4,
    reconnectPeriod: 0,
  });

  client.on('connect', () => {
    console.log(`[azure] підключено до ${HostName} як ${DeviceId}, шлю телеметрію кожні ${BATCH_MS}мс`);
  });

  client.on('error', (err) => console.error('[azure] помилка MQTT:', err.message));

  client.on('close', () => {
    console.log('[azure] з\'єднання закрито, перепідключення через 2с…');
    setTimeout(connect, 2000);
  });

  setTimeout(() => client.end(true), (TOKEN_TTL_S - 60) * 1000);
}

connect();

setInterval(() => {
  if (!client || !client.connected) return;
  const msg = makeMessage();
  client.publish(topic, JSON.stringify(msg), { qos: 0 }, (err) => {
    if (err) console.error('[azure] не вдалося надіслати:', err.message);
  });
}, BATCH_MS);
