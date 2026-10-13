#!/usr/bin/env node
const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
    return acc;
  }, [])
);

const URL_BASE = args.url || 'http://localhost:3000';
const MODE = args.mode || 'json';
const DEVICE = args.id || 'mock-01';
const API_KEY = process.env.API_KEY || '';

const FS = 250;          // Гц
const BATCH = 50;
const BATCH_MS = 200;    // мс між пакетами
const VITALS_EVERY = 5;

const VITALS = { hr: 72, spo2: 98, temp: 36.6, battery: 87 };

// Один серцевий цикл (72 уд/хв при 250 Гц), шлеться по колу
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
  const msg = { device_id: DEVICE, fs: FS, ecg };
  if (batch++ % VITALS_EVERY === 0) Object.assign(msg, VITALS);
  return msg;
}

function toLine(m) {
  const parts = [`id=${m.device_id}`];
  for (const k of ['hr', 'spo2', 'temp', 'battery']) if (m[k] !== undefined) parts.push(`${k}=${m[k]}`);
  parts.push(`fs=${m.fs}`, `ecg=${m.ecg.join(',')}`);
  return parts.join(' ');
}

async function send() {
  const m = makeMessage();
  const isLine = MODE === 'line';
  try {
    const r = await fetch(`${URL_BASE}/api/${isLine ? 'line' : 'data'}`, {
      method: 'POST',
      headers: { 'Content-Type': isLine ? 'text/plain' : 'application/json', ...(API_KEY && { 'X-API-Key': API_KEY }) },
      body: isLine ? toLine(m) : JSON.stringify(m),
    });
    if (!r.ok) console.error('сервер відповів', r.status, await r.text());
  } catch (e) {
    console.error('не вдалося надіслати:', e.message);
  }
}

console.log(`Пересилання даних на ${URL_BASE} (режим ${MODE}, пристрій ${DEVICE}): ` +
  `HR ${VITALS.hr}, SpO2 ${VITALS.spo2}, T ${VITALS.temp}, батарея ${VITALS.battery}%.`);
setInterval(send, BATCH_MS);
