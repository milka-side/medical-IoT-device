// Заготовка під MQTT: wearable/<device_id>/data
function startMqttIngest(hub) {
  const url = process.env.MQTT_URL;
  if (!url) return null;
  console.log(`[mqtt] MQTT_URL задано (${url}), але інтеграція ще не реалізована`);
  return null;
}

module.exports = { startMqttIngest };
