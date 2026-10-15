const mqtt = require('mqtt');
const { parseLine } = require('./parser');

function startMqttIngest(hub) {
  const url = process.env.MQTT_URL;
  if (!url) {
    console.log('[mqtt] MQTT_URL не задано — приймання з MQTT вимкнено');
    return null;
  }
  const topic = process.env.MQTT_TOPIC || 'wearable/+/data';

  const client = mqtt.connect(url, {
    username: process.env.MQTT_USERNAME || undefined,
    password: process.env.MQTT_PASSWORD || undefined,
  });

  client.on('connect', () => {
    client.subscribe(topic, (err) => {
      if (err) console.error(`[mqtt] не вдалося підписатись на ${topic}:`, err.message);
      else console.log(`[mqtt] підключено до ${url}, підписка на ${topic}`);
    });
  });

  client.on('message', (_topic, payload) => {
    const text = payload.toString('utf8').trim();
    if (!text) return;
    const data = parseLine(text);
    if (!data) {
      console.error('[mqtt] не вдалося розібрати повідомлення:', text.slice(0, 80));
      return;
    }
    const res = hub.ingest(data, 'mqtt');
    if (!res.ok) console.warn('[mqtt] відкинуто повідомлення:', res.error);
  });

  client.on('error', (err) => console.error('[mqtt] помилка:', err.message));
  client.on('reconnect', () => console.log('[mqtt] перепідключення…'));

  return client;
}

module.exports = { startMqttIngest };
