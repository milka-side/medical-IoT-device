// Dashboard commands as Azure direct method "command" ({"commands":["all"]}).
// The device answers 200 or 4xx with a reason.
// Needs the service connection string: AZURE_IOTHUB_CONNECTION_STRING, or
// built from AZURE_DEVICE_CONNECTION_STRING (host) and
// AZURE_EVENTHUB_CONNECTION_STRING (iothubowner key).
const { Client } = require('azure-iothub');

const METHOD = 'command';

// all_force = "Виміряти все одно" (measure during warm-up).
const VALID_TYPES = new Set(['ecg', 'vitals', 'temp', 'all', 'all_force', 'sd_upload']);

// Refusal reasons from the firmware (on_azure_command).
const REASONS = {
  warmup: 'датчики ще прогріваються після ввімкнення — натисни «Виміряти все одно»',
  measuring: 'зараз іде вимірювання — SD-картку можна прочитати після нього',
};

let client = null;
let deviceId = null;

function parseCs(cs) {
  const out = {};
  for (const part of String(cs || '').split(';')) {
    const i = part.indexOf('=');
    if (i > 0) out[part.slice(0, i).trim()] = part.slice(i + 1).trim();
  }
  return out;
}

function serviceConnectionString() {
  if (process.env.AZURE_IOTHUB_CONNECTION_STRING) return process.env.AZURE_IOTHUB_CONNECTION_STRING;
  const dev = parseCs(process.env.AZURE_DEVICE_CONNECTION_STRING);
  const eh = parseCs(process.env.AZURE_EVENTHUB_CONNECTION_STRING);
  if (!dev.HostName || !eh.SharedAccessKeyName || !eh.SharedAccessKey) return null;
  return `HostName=${dev.HostName};SharedAccessKeyName=${eh.SharedAccessKeyName};SharedAccessKey=${eh.SharedAccessKey}`;
}

function startAzureCommands() {
  const cs = serviceConnectionString();
  deviceId = process.env.AZURE_COMMAND_DEVICE_ID || parseCs(process.env.AZURE_DEVICE_CONNECTION_STRING).DeviceId;
  if (!cs || !deviceId) {
    console.log('[azure] немає даних для direct methods — кнопки на сайті не працюватимуть');
    return false;
  }
  client = Client.fromConnectionString(cs);
  console.log(`[azure] команди з сайту йдуть через direct method "${METHOD}" на пристрій ${deviceId}`);
  return true;
}

// null = not configured, otherwise { delivered, status, reason, error }.
async function sendCommand(type) {
  if (!client) return null;
  try {
    const { result } = await client.invokeDeviceMethod(deviceId, {
      methodName: METHOD,
      payload: { commands: [type] },
      responseTimeoutInSeconds: 10,
      connectTimeoutInSeconds: 5,
    });
    const payload = result && result.payload ? result.payload : {};
    return {
      delivered: true,
      status: result ? result.status : 0,
      reason: REASONS[payload.reason] || payload.reason || '',
    };
  } catch (e) {
    return { delivered: false, error: e.message };
  }
}

module.exports = { startAzureCommands, sendCommand, VALID_TYPES };
