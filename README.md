<h1 align="center">Medical IoT Device</h1>
<p align="center"><i>Проєкт з курсу «Принципи організації комп’ютера»</i></p>
<div align="center">
  <a href="https://github.com/Sviat-Vol">Святослав Волянюк</a>,
  <a href="https://github.com/alinlinss">Аліна Дерев'ягіна</a>,
  <a href="https://github.com/SherbynaEugene">Євгеній Щербина</a>,
  <a href="https://github.com/milka-side">Мілана Катрич</a>

<a href="mailto:yevgeniy@karplyuk.kiev.ua">Ментор: Євгеній Карплюк</a>
</div>


---

Medical IoT Device - це компактний носимий IoT-пристрій, який вимірює базові життєві показники: **ЕКГ**, **пульс**, **насичення крові киснем (SpO₂)** і **температуру тіла**. Пристрій обробляє їх на мікроконтролері й показує у вебпанелі майже в реальному часі.

---

## Зміст
- [Огляд](#огляд)
- [Можливості](#можливості)
- [Структура репозиторію](#структура-репозиторію)
- [Перелік компонентів](#перелік-компонентів)
- [Прошивка](#прошивка)
- [Запуск](#запуск)
- [Azure IoT Hub](#azure-iot-hub)
- [Формат даних](#формат-даних)
- [План розвитку](#план-розвитку)
- [Контакти](#контакти)
- [Джерела](#джерела)

---

## Огляд

### Про що наш проєкт?
Мета проєкту - створити невеликий носимий пристрій з автономним живленням, який збирає життєві показники, виконує первинну обробку даних та передає результати до системи моніторингу. Дані доступні оператору у вебінтерфейсі майже в реальному часі. Така система корисна для навчання й демонстрації віддаленого моніторингу пацієнтів, але не призначена для ухвалення медичних рішень.

### Показники, що вимірюються
| Показник    | Сенсор   | Опис                                                       |
|-------------|----------|------------------------------------------------------------|
| ЕКГ         | ADS1293  | Сирі відліки ЕКГ, що передаються пакетами кодів АЦП.       |
| Пульс       | MAX30102 | Обчислюється на пристрої, відображається в уд/хв.          |
| SpO₂        | MAX30102 | Оцінюється за фотоплетизмографічним сигналом, у %.         |
| Температура | TMP117   | Цифрове вимірювання температури тіла, у °C.                |

### Архітектура системи
```
 XIAO ESP32-C6 + сенсори ──Wi-Fi, MQTT──▶ Azure IoT Hub ──▶ сервер (Node.js) ──WebSocket──▶ вебпанель
                         ◀──direct methods (команди)──
```

### Стан проєкту
- **Поточна версія:** Proof of concept.
- **Стан розробки:** Активна розробка.
- **Останнє оновлення:** 14.10.2026

### Команда
- [Святослав Волянюк](https://github.com/Sviat-Vol)
- [Аліна Дерев'ягіна](https://github.com/alinlinss)
- [Євгеній Щербина](https://github.com/SherbynaEugene)
- [Мілана Катрич](https://github.com/milka-side)

Ментор: Євгеній Карплюк.

---

## Можливості

### Апаратна частина
- Seeed XIAO ESP32-C6 як основний контролер із Wi-Fi.
- Аналоговий ЕКГ (ADS1293) по SPI; пульсоксиметр (MAX30102) і датчик температури (TMP117) по I²C.
- LiPo-акумулятор із заряджанням для автономної роботи.
- Підготовлено принципову схему й 3D-модель корпуса (Autodesk Fusion).

### Програмна частина
- Пристрій передає телеметрію лише через Azure IoT Hub; сервер на Node.js читає її з хабу, перевіряє та відкидає значення поза допустимими межами. Команди з вебпанелі йдуть на пристрій як direct methods Azure.
- Миттєва розсилка даних усім відкритим браузерам через WebSocket.
- Вебпанель із живим графіком ЕКГ і плитками пульсу, SpO₂ та температури, які підсвічують значення поза нормою, а також позначає, коли дані від пристрою давно не оновлювались.
- Віртуальний пристрій із тестовими даними, що шле їх в Azure IoT Hub, для демонстрації без апаратної частини.
- Необов'язкова перевірка ключа API (`X-API-Key`) для передачі файлів із SD-картки.

### Дизайн
- Носимий корпус для 3D-друку, змодельований в Autodesk Fusion.

---

## Структура репозиторію
```
.
├── README.md
├── web
│   ├── server.js
│   ├── src
│   └── public
├── tools
│   └── azure-mock-sender.js
└── cad_model
    ├── main_case_main_part.stl
    ├── main_case_lower_cap.stl
    └── main_case_upper_cap.stl
```

### Опис тек

- **web/**: Сервер моніторингу та вебпанель:
    - **server.js**: Вебпанель, API для неї і WebSocket-сервер.
    - **src/**: Приймання даних з Azure IoT Hub, команди на пристрій, перевірка даних, розсилка, історія вимірювань.
    - **public/**: Сторінка вебпанелі (HTML, CSS, JS).
- **tools/**: Допоміжні інструменти:
    - **azure-mock-sender.js**: Віртуальний пристрій, що шле дані в Azure IoT Hub.
- **cad_model/**: STL-файли корпуса для 3D-друку.

Цей розділ оновлюватиметься в міру додавання файлів.

---

## Перелік компонентів

### Таблиця компонентів

| Категорія       | Компонент           | Специфікація          | Кількість | Орієнтовна вартість | Примітки                         |
|-----------------|---------------------|-----------------------|-----------|---------------------|----------------------------------|
| **Електроніка** |                     |                       |           |                     |                                  |
|                 | Мікроконтролер      | Seeed XIAO ESP32-C6   | 1         | -                   | Wi-Fi, первинна обробка          |
|                 | ЕКГ        | TI ADS1293            | 1         | -                   | SPI                              |
|                 | Пульсоксиметр       | MAX30102              | 1         | -                   | I²C, пульс і SpO₂                |
|                 | Датчик температури  | TI TMP117             | 1         | -                   | I²C                              |
|                 | Перетворювач рівнів | 3,3 В ↔ 5 В           | 1         | -                   | Лише якщо потрібно модулю        |
| **Живлення**    |                     |                       |           |                     |                                  |
|                 | Акумулятор          | LiPo                  | 1         | -                   | Із заряджанням                   |
| **Інше**        |                     |                       |           |                     |                                  |
|                 | Корпус              | 3D-друк               | 1         | -                   | Модель в Autodesk Fusion         |

**Загальна орієнтовна вартість**: буде розраховано.

---

## Прошивка

У роботі. Прошивку ESP32-C6 для зчитування реальних сенсорів ще не завершено, тому повний шлях від фізичних сенсорів до вебпанелі поки не перевірено. Частоту дискретизації та точність вимірювань ще не підтверджено вимірюваннями.

---

## Запуск

Потрібен Node.js версії 18 або новішої.

```bash
cd web
npm install
npm run start:cloud   # бере рядки підключення Azure з web/.azure.env
# вебпанель: http://localhost:3000
```

Як отримати рядки підключення і що покласти в `web/.azure.env`, описано в розділі [Azure IoT Hub](#azure-iot-hub).

Демонстрація без апаратної частини (в іншому терміналі, з каталогу `web/`):

```bash
AZURE_DEVICE_CONNECTION_STRING="<connection string пристрою>" npm run mock
```

Змінні середовища: `PORT` (за замовчуванням 3000), `HOST` (за замовчуванням 0.0.0.0), `API_KEY` (вмикає перевірку `X-API-Key` для передачі файлів із SD-картки).

---

## Azure IoT Hub

Єдиний шлях даних: `пристрій → Azure IoT Hub (MQTT) → сервер (web/, через вбудований Event Hub-
сумісний ендпоінт) → сайт`. Кнопки на сайті працюють у зворотний бік: сервер викликає на пристрої direct
method `command` (`web/src/azure-commands.js`), і пристрій одразу відповідає, чи виконає команду.
Код приймання - `web/src/azure-ingest.js`, симулятор пристрою - `tools/azure-mock-sender.js`.

Плата ESP32-C6 вміє слати телеметрію в хаб напряму (`firmware/main/telemetry_azure.c`): MQTT на порту
8883, TLS (перевірка сертифіката через вбудований у ESP-IDF certificate bundle) і SAS-токен, який плата сама
генерує з device connection string після синхронізації часу по SNTP і оновлює кожні ~24 години.

### 1. Створити хаб і пристрій (Azure CLI)

Потрібен встановлений [Azure CLI](https://learn.microsoft.com/cli/azure/install-azure-cli) і `az login`.

```bash
# змінні, можна підставити свої назви (HUB_NAME має бути унікальним у всьому Azure)
RG=medical-iot-rg
HUB_NAME=medical-iot-hub-<щось-унікальне>
DEVICE_ID=esp32c6-01
LOCATION=westeurope

# розширення для роботи з IoT Hub
az extension add --name azure-iot

# група ресурсів
az group create --name $RG --location $LOCATION

# сам IoT Hub (F1 — безкоштовний тір, один на підписку)
az iot hub create --name $HUB_NAME --resource-group $RG --sku F1 --partition-count 2

# реєстрація пристрою (симулятора / майбутньої плати)
az iot hub device-identity create --hub-name $HUB_NAME --device-id $DEVICE_ID

# рядок підключення ПРИСТРОЮ — для tools/azure-mock-sender.js
az iot hub device-identity connection-string show --hub-name $HUB_NAME --device-id $DEVICE_ID --output tsv

# рядок підключення Event Hub-сумісного ендпоінту — для сервера (web/)
az iot hub connection-string show --hub-name $HUB_NAME --default-eventhub --output tsv

# назва Event Hub-сумісного ендпоінту (EventHubName для сервера)
az iot hub show --name $HUB_NAME --query "properties.eventHubEndpoints.events.path" --output tsv
```

Те саме можна зробити в [Azure Portal](https://portal.azure.com): створити ресурс **IoT Hub** → у ньому
**Devices → + Add Device** (дає connection string пристрою) → **Built-in endpoints** (дає Event Hub-compatible
endpoint і Event Hub-compatible name для сервера).

### 2. Запустити

У двох терміналах з `web/`:

```bash
# термінал 1 — сервер, приймає дані з Azure
AZURE_EVENTHUB_CONNECTION_STRING="<Event Hub-compatible endpoint з кроку 1>" \
AZURE_EVENTHUB_NAME="<Event Hub-compatible name з кроку 1>" \
npm start

# термінал 2 — симулятор пристрою, шле дані в Azure IoT Hub по MQTT
AZURE_DEVICE_CONNECTION_STRING="<connection string пристрою з кроку 1>" \
npm run mock
```

Відкрити http://localhost:3000 — мають з'явитися дані від симулятора.

Ті самі змінні можна покласти у `web/.azure.env` (він у `.gitignore`) і запускати сервер через `npm run start:cloud`.

### 3. Підключити плату ESP32-C6

Створити `firmware/sdkconfig.defaults.local` (він у `.gitignore`, ключ не потрапить у git):

```
CONFIG_MIOT_AZURE_ENABLE=y
CONFIG_MIOT_AZURE_CONNECTION_STRING="HostName=<hub>.azure-devices.net;DeviceId=esp32c6-01;SharedAccessKey=<key>"
```

Потім перезібрати з чистим `sdkconfig` (defaults застосовуються лише до ключів, яких у `sdkconfig` ще немає):

```bash
cd firmware
rm -f sdkconfig
idf.py build flash monitor
```

Або те саме через `idf.py menuconfig` → Medical IoT Device → *Send telemetry to Azure IoT Hub*.
У лозі має з'явитися `telemetry_azure: connected to Azure IoT Hub ...`.

Окремо по HTTP йдуть лише файли з SD-картки (кнопка «Завантажити дані з SD»): плата надсилає їх напряму на
сервер (`CONFIG_MIOT_SERVER_URL`, `POST /api/sd-upload`), бо цілий файл завеликий для тіра F1.

Щоб не вибрати ліміт безкоштовного тіра F1 (8000 повідомлень на добу), у режимі очікування в Azure йде
лише одне статусне повідомлення на хвилину (`MIOT_AZURE_IDLE_PERIOD_S`); під час вимірювання дані
надсилаються щосекунди.

---

## Формат даних

Пристрій надсилає в Azure IoT Hub повідомлення у форматі JSON:

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

| Поле        | Опис                                       |
|-------------|--------------------------------------------|
| `device_id` | Ідентифікатор пристрою                     |
| `ecg`       | Пакет відліків ЕКГ (коди АЦП)              |
| `fs`        | Частота дискретизації ЕКГ, Гц (за замовчуванням 250) |
| `hr`        | Пульс, уд/хв                               |
| `spo2`      | Насичення крові киснем, %                  |
| `temp`      | Температура тіла, °C                       |
| `battery`   | Заряд акумулятора, %                       |

Усі поля необов'язкові.

| Метод | Шлях          | Опис                               |
|-------|---------------|------------------------------------|
| POST  | `/api/command` | Команда з вебпанелі на пристрій (через Azure) |
| POST  | `/api/sd-upload` | Файл із SD-картки від пристрою (частинами) |
| GET   | `/api/latest` | Останні значення для кожного пристрою |
| GET   | `/api/health` | Стан сервера                       |
| WS    | `/ws`         | Потік даних для вебпанелі          |

---

## План розвитку

### Виконано
- Вебсервер, HTTP API і WebSocket.
- Вебпанель, з позначенням застарілих даних пристрою.
- Передача даних і команд через Azure IoT Hub, перевірено на реальній платі.
- Віртуальний пристрій для Azure IoT Hub.
- Архітектура, принципова схема та 3D-модель корпуса.

### Наступні кроки
1. Написати код для сенсорів для ESP32-C6 і перевірити частоту дискретизації.
2. Перевірити стабільність вимірювань.
3. Додати обробку в Azure Functions.
4. Протестувати роботу при втраті мережі, пошкоджених пакетах і від'єднанні сенсорів.
5. Потім: Node-RED і Grafana, аналіз HRV та аритмій, виявлення гіпоксії.

---

## Контакти

### Розробники
Студенти факультету прикладних наук Українського католицького університету:
- Святослав Волянюк - [volianiuk.pn@ucu.edu.ua](mailto:volianiuk.pn@ucu.edu.ua);
- Аліна Дерев'ягіна - [derevyagina.pn@ucu.edu.ua](mailto:derevyagina.pn@ucu.edu.ua);
- Євгеній Щербина - [shcherbyna.pn@ucu.edu.ua](mailto:shcherbyna.pn@ucu.edu.ua);
- Мілана Катрич - [katrych.pn@ucu.edu.ua](mailto:katrych.pn@ucu.edu.ua).

### Ментор
- Євгеній Карплюк - [yevgeniy@karplyuk.kiev.ua](mailto:yevgeniy@karplyuk.kiev.ua).

---

## Джерела
- [ESP32-C6](https://www.espressif.com/en/products/socs/esp32-c6), Espressif Systems
- [ADS1293](https://www.ti.com/product/ADS1293), Texas Instruments
- [MAX30102](https://www.analog.com/en/products/max30102.html), Analog Devices
- [TMP117](https://www.ti.com/product/TMP117), Texas Instruments
- [Документація Node.js](https://nodejs.org/docs/)
