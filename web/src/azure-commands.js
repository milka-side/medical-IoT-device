// Dashboard commands to the device over Azure IoT Hub direct methods: the
// server calls method "command" on the device with {"commands":["all"]} and
// gets an immediate answer - accepted (200) or refused with a reason (4xx) -
// or an error if the device isn't connected to the hub.
//
// Needs the hub's service connection string (policy iothubowner):
// AZURE_IOTHUB_CONNECTION_STRING, or derived from what .azure.env already
// holds - the hub host from AZURE_DEVICE_CONNECTION_STRING and the policy
// key from AZURE_EVENTHUB_CONNECTION_STRING (the built-in Event Hub endpoint
// is signed with the same iothubowner key).
const { Client } = require('azure-iothub');

const METHOD = 'command';

// all_force = "Виміряти все одно": measure even during the power-on warm-up.
const VALID_TYPES = new Set(['ecg', 'vitals', 'temp', 'all', 'all_force', 'sd_upload']);

// Refusal codes the firmware sends back (main.c on_azure_command).
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
    console.log('[azure] немає даних для direct methods — команди йдуть лише через HTTP');
    return false;
  }
  client = Client.fromConnectionString(cs);
  console.log(`[azure] команди з сайту йдуть через direct method "${METHOD}" на пристрій ${deviceId}`);
  return true;
}

// null = Azure commands not configured. Otherwise { delivered, status, reason, error }.
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
