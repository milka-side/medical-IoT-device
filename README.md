# Medical IoT Device

Прототип носимого медичного пристрою, який у реальному часі вимірює **ЕКГ, пульс, SpO₂ і температуру**
та показує їх на веб-панелі.

## Можливості

- ЕКГ у реальному часі
- Пульс, SpO₂ і температура з підсвіткою значень поза нормою
- Дані надходять через Wi-Fi (HTTP REST API або MQTT) або USB (Serial); опційно — через Azure IoT Hub

## Архітектура

```
 ┌──────────────────────────┐  HTTP REST │               │   WS   │           │
 │  XIAO ESP32-C6           │ ─────────▶ │               │ ─────▶ │  Браузер  │
 │  ADS1293   (ЕКГ)         │  MQTT      │  Сервер       │        │  панель   │
 │  MAX30102  (пульс, SpO₂) │ ─────────▶ │  Node.js      │        │           │
 │  MAX30205  (температура) │  Serial    │  web/         │        │           │
 │                          │ ─────────▶ │               │        │           │
 └──────────────────────────┘            └───────────────┘        └───────────┘
            │
            │ MQTT (device-to-cloud)
            ▼
   ┌─────────────────┐   Event Hub-сумісний
   │  Azure IoT Hub   │ ──────────────────▶  (той самий сервер приймає і звідси)
   └─────────────────┘
```

Мікроконтролер фільтрує сигнал і рахує пульс, а сервер приймає дані одним із трьох шляхів (HTTP REST API,
MQTT-брокер або Azure IoT Hub) і миттєво передає їх у браузер через WebSocket.

## Апаратна частина

| Компонент            | Призначення                      |
|----------------------|----------------------------------|
| Seeed XIAO ESP32-C6  | мікроконтролер, Wi-Fi            |
| ADS1293              | аналоговий фронтенд ЕКГ          |
| MAX30102             | пульс і SpO₂ (пульсоксиметрія)   |
| MAX30205 / DS18B20  | температура тіла                 |
| LiPo                 | живлення                         |

## Швидкий старт

Потрібен Node.js 18+

```bash
cd web
npm install
npm start
```

Демо без мікроконтроллера (у другому терміналі, з `web`):

```bash
npm run mock
```

Пристрій через USB:

```bash
pip install pyserial
python tools/serial-bridge.py --port /dev/ttyACM0
```

## Формат даних

Пристрій надсилає `POST /api/data`:

```json
{
  "device_id": "esp32c6-01",
  "fs": 250,
  "ecg": [2048, 2051, 2060, 2210, 2850, 2300, 2040],
  "hr": 72,
  "spo2": 98,
  "temp": 36.6,
  "battery": 87
}
```

Або текстовий рядок через Serial / `POST /api/line`:

```
id=esp32c6-01 hr=72 spo2=98 temp=36.6 ecg=2048,2051,2060
```

| Поле      | Одиниці | Діапазон |
|-----------|---------|----------|
| `hr`      | уд/хв   | 20–300   |
| `spo2`    | %       | 50–100   |
| `temp`    | °C      | 25–45    |
| `battery` | %       | 0–100    |
| `ecg`     | коди АЦП | до 5000 відліків |

### API

| Метод | Шлях          | Опис |
|-------|---------------|------|
| POST  | `/api/data`   | дані від пристрою (JSON) |
| POST  | `/api/line`   | дані від пристрою (текст) |
| GET   | `/api/latest` | останні значення по кожному пристрою |
| GET   | `/api/health` | стан сервера |
| WS    | `/ws`         | живий потік для браузера |

Налаштування: `PORT` (3000), `HOST` (0.0.0.0), `API_KEY` (вмикає перевірку заголовка `X-API-Key`).

## MQTT

Другий шлях даних (додатковий, не замінює прямий HTTP): пристрій публікує телеметрію на звичайний
MQTT-брокер у тему `wearable/<device_id>/data`, сервер підписується на неї й так само передає дані в
браузер. Корисне навантаження — той самий JSON/текстовий рядок, що й для HTTP `/api/data` / `/api/line`.

Поки немає реальної плати ESP32, роль пристрою імітує `tools/mqtt-mock-sender.js`.

```bash
# термінал 1
MQTT_URL="mqtt://broker.hivemq.com:1883" npm start

# термінал 2
npm run mock:mqtt
```

За замовчуванням обидва використовують публічний тестовий брокер **broker.hivemq.com** — зручно для
демо без власної інфраструктури, але дані там публічні й не для продакшн-використання. Для власного
брокера (Mosquitto, EMQX тощо) — задати свій `MQTT_URL` (і за потреби `MQTT_USERNAME`/`MQTT_PASSWORD`,
`MQTT_TOPIC`) в обох командах.

Перевірка, що дані дійшли (третій термінал):

```bash
curl http://localhost:3000/api/latest
```

HTTP і MQTT можна тримати увімкненими одночасно — `MQTT_URL` просто додається до запуску сервера, і
`npm run mock` та `npm run mock:mqtt` можна запускати паралельно (у різних терміналах): обидва пристрої
(`mock-01` і `mock-mqtt-01`) з'являться на панелі одночасно, бо в них різні `device_id`.

## Azure IoT Hub

Третій, опційний шлях даних: `пристрій → Azure IoT Hub (MQTT) → сервер (web/, через вбудований Event Hub-
сумісний ендпоінт) → сайт`. Код приймання (`web/src/azure-ingest.js`) і симулятор пристрою
(`tools/azure-mock-sender.js`) повністю готові й покривають той самий формат даних, що й HTTP/MQTT.

> **Статус:** використання Azure IoT Hub ще під питанням — команда наразі в процесі імплементації цього
> рішення в проєкт. Код приймання даних (`azure-ingest.js`) і симулятор пристрою (`azure-mock-sender.js`)
> вже готові; залишається підключити сам хаб.

### 1. Створити хаб і пристрій (Azure CLI)

Потрібен встановлений [Azure CLI](https://learn.microsoft.com/cli/azure/install-azure-cli) і `az login`.

```bash
RG=medical-iot-rg
HUB_NAME=medical-iot-hub-<щось-унікальне>
DEVICE_ID=esp32c6-01
LOCATION=westeurope

az extension add --name azure-iot

az group create --name $RG --location $LOCATION

az iot hub create --name $HUB_NAME --resource-group $RG --sku F1 --partition-count 2

az iot hub device-identity create --hub-name $HUB_NAME --device-id $DEVICE_ID

az iot hub device-identity connection-string show --hub-name $HUB_NAME --device-id $DEVICE_ID --output tsv

az iot hub connection-string show --hub-name $HUB_NAME --default-eventhub --output tsv

az iot hub show --name $HUB_NAME --query "properties.eventHubEndpoints.events.path" --output tsv
```

Те саме можна зробити в [Azure Portal](https://portal.azure.com): створити ресурс **IoT Hub** → у ньому
**Devices → + Add Device** (дає connection string пристрою) → **Built-in endpoints** (дає Event Hub-compatible
endpoint і Event Hub-compatible name для сервера).

### 2. Запустити

У двох терміналах з `web/`:

```bash
# термінал 1
AZURE_EVENTHUB_CONNECTION_STRING="<Event Hub-compatible endpoint з кроку 1>" \
AZURE_EVENTHUB_NAME="<Event Hub-compatible name з кроку 1>" \
npm start

# термінал 2
AZURE_DEVICE_CONNECTION_STRING="<connection string пристрою з кроку 1>" \
npm run mock:azure
```

Відкрити http://localhost:3000 — дані від симулятора мають з'явитися так само, як і від `npm run mock`.

Коли з'явиться справжня плата ESP32, вона підключатиметься до того ж IoT Hub по MQTT з device connection
string, і дані так само дійдуть до сервера без жодних змін на його боці.

## Структура проєкту

```
medical-iot-device/
├── web/
│   ├── server.js          HTTP-сервер, REST API, WebSocket
│   ├── src/               нормалізація даних, розсилка, приймання з MQTT і з Azure IoT Hub
│   └── public/            веб-панель (HTML, CSS, JS)
├── tools/
│   ├── mock-sender.js       віртуальний пристрій для демо (прямий HTTP REST)
│   ├── mqtt-mock-sender.js  віртуальний пристрій, що шле дані на MQTT-брокер
│   ├── azure-mock-sender.js віртуальний пристрій, що шле дані в Azure IoT Hub по MQTT
│   └── serial-bridge.py     міст USB Serial → сервер
└── docs/                  зображення для README
```

## План проєкту

- [x] Веб-панель реального часу
- [x] Прийом даних через HTTP REST API і Serial
- [x] MQTT — приймання готове і перевірено наживо (сервер + `mqtt-mock-sender.js` через публічний брокер)
- [ ] Хмара (Azure IoT Hub) — під питанням, команда ще в процесі імплементації цього рішення;
      код приймання готовий і працює з симулятором `azure-mock-sender.js` (див. розділ "Azure IoT Hub")
- [ ] Прошивка ESP32: зчитування датчиків і відправка по Wi-Fi
- [ ] Node-RED / Grafana поверх MQTT
- [ ] Аналіз аритмій і HRV, виявлення гіпоксії
- [ ] Корпус (Fusion) і друкована плата (KiCad)
