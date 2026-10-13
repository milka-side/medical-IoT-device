const ECG_SECONDS = 5;            // ширина вікна ЕКГ, с
const ALARMS = {                  // межі підсвітки (демо, не медичні)
  hr: (v) => v < 50 || v > 120,
  spo2: (v) => v < 92,
  temp: (v) => v < 35 || v > 37.8,
};

const $ = (id) => document.getElementById(id);
const canvas = $('ecg');
let fs = 250;
let ecg = [];

function showVitals(d) {
  for (const [key, digits] of [['hr', 0], ['spo2', 0], ['temp', 1]]) {
    if (d[key] === undefined) continue;
    $(key).textContent = d[key].toFixed(digits);
    $('tile-' + key).classList.toggle('alarm', ALARMS[key](d[key]));
  }
}

function addEcg(samples, rate) {
  fs = rate || fs;
  ecg.push(...samples);
  const max = fs * ECG_SECONDS;
  if (ecg.length > max) ecg = ecg.slice(-max);
}

function drawEcg() {
  const dpr = window.devicePixelRatio || 1;
  const w = (canvas.width = canvas.clientWidth * dpr);
  const h = (canvas.height = canvas.clientHeight * dpr);
  const ctx = canvas.getContext('2d');
  if (ecg.length < 2) return;

  const lo = Math.min(...ecg), hi = Math.max(...ecg);
  const span = hi - lo || 1;
  const n = fs * ECG_SECONDS;

  ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue('--hr');
  ctx.lineWidth = 2 * dpr;
  ctx.beginPath();
  ecg.forEach((v, i) => {
    const x = (i / (n - 1)) * w;
    const y = h * 0.9 - ((v - lo) / span) * h * 0.8;
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.stroke();
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/ws`);
  const conn = $('conn');
  ws.onopen = () => { conn.textContent = 'підключено'; conn.className = 'conn on'; };
  ws.onclose = () => {
    conn.textContent = 'не підключено';
    conn.className = 'conn off';
    setTimeout(connect, 2000);
  };
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.type === 'snapshot') Object.values(m.devices).forEach(showVitals);
    if (m.type === 'data') {
      showVitals(m.data);
      if (m.data.ecg) addEcg(m.data.ecg, m.data.fs);
    }
  };
}

connect();
(function loop() {
  drawEcg();
  requestAnimationFrame(loop);
})();
