const { EventHubConsumerClient } = require('@azure/event-hubs');

function startAzureIngest(hub) {
  const connectionString = process.env.AZURE_EVENTHUB_CONNECTION_STRING;
  const eventHubName = process.env.AZURE_EVENTHUB_NAME;
  const consumerGroup = process.env.AZURE_EVENTHUB_CONSUMER_GROUP || '$Default';

  if (!connectionString || !eventHubName) {
    console.log('[azure] AZURE_EVENTHUB_CONNECTION_STRING / AZURE_EVENTHUB_NAME не задано — приймання з Azure IoT Hub вимкнено');
    return null;
  }

  const client = new EventHubConsumerClient(consumerGroup, connectionString, eventHubName);

  const subscription = client.subscribe({
    processEvents: async (events) => {
      for (const event of events) {
        let data;
        try {
          data = typeof event.body === 'string' ? JSON.parse(event.body) : event.body;
        } catch (e) {
          console.error('[azure] не вдалося розібрати повідомлення:', e.message);
          continue;
        }
        if (data && typeof data === 'object' && data.device_id === undefined) {
          data.device_id = event.systemProperties?.['iothub-connection-device-id'];
        }
        const res = hub.ingest(data, 'azure');
        if (!res.ok) console.warn('[azure] відкинуто повідомлення:', res.error);
      }
    },
    processError: async (err) => {
      console.error('[azure] помилка Event Hub:', err.message);
    },
  });

  console.log(`[azure] приймання телеметрії з Azure IoT Hub увімкнено (consumer group: ${consumerGroup})`);
  return subscription;
}

module.exports = { startAzureIngest };
