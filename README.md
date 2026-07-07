# ESP32 Control Hub

Управление 2× SG90 сервоприводами и 2× WS2812B кольцами (по 7 LED) через веб-интерфейс с **WebSocket real-time** связью.

## ⚡ Возможности

- 🎛️ **2 сервопривода** — слайдеры 0-180°, кнопки пресетов, вкл/выкл
- 🌈 **14 LED адресных** — 9 анимаций, color picker, яркость, скорость
- 🔌 **Real-time WebSocket** — мгновенная реакция (<50мс)
- 📱 **Адаптивный UI** — glassmorphism, тёмная тема, неоновые акценты
- 🔁 **Auto-reconnect** — переподключение при потере связи
- 📡 **WiFi AP** — работает без роутера

## 🔌 Подключение

```
12V ──→ Mini560 ──→ 5V ──┬── Servo 1 VCC (red)
                         ├── Servo 2 VCC (red)
                         ├── WS2812B ring 1 VCC
                         ├── WS2812B ring 2 VCC
                         ├── 470μF capacitor (для серво)
                         └── 1000μF capacitor (для LED)

ESP32-C3                Servo 1           Servo 2
GPIO2  ────────────────  Signal (orange)
GPIO3  ──────────────────────────────────  Signal (orange)
GND    ──┬── Servo 1 GND (brown)
         ├── Servo 2 GND (brown)
         ├── Mini560 GND
         └── 12V source GND

ESP32-C3                WS2812B
GPIO10 ──→ DIN ring 1 ──→ DOUT ring 1 ──→ DIN ring 2 (daisy chain)
GND    ────→ GND rings
```

**⚠️ КРИТИЧНО:**
1. **Общая GND** между ESP32-C3, Mini560, 12V источником, серво и LED — без этого не работает
2. **Конденсаторы** на 5V выходе Mini560:
   - 470μF электролитический для серво (защита от просадок)
   - 1000μF электролитический для LED (броски тока при заливке белым)
3. **Серво питание** — ТОЛЬКО от 5V Mini560, не от 3.3V ESP (brownout!)
4. **WS2812B** — DIN первого кольца → GPIO10, DOUT первого → DIN второго (daisy chain = 14 LED)

## 📦 Сборка

### Arduino IDE

1. Установить поддержку ESP32-C3:
   - File → Preferences → Additional boards manager URLs
   - Добавить: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board → Boards Manager → найти "esp32" → Install
2. Выбрать плату: **Tools → Board → ESP32C3 Dev Module**
3. Настроить:
   - Flash Size: **4MB (32Mb)**
   - Upload Speed: **921600**
   - Partition Scheme: **Default 4MB with spiffs**
4. Установить библиотеки (Tools → Manage Libraries):
   - `Adafruit NeoPixel` (>=1.12.0)
   - `ESPAsyncWebServer` (by ESP32Async)
   - `AsyncTCP` (by ESP32Async)
   - `ESP32Servo` (>=3.0.0)
   - `ArduinoJson` (>=6.21.0)
   - `WebSockets` by Markus Sattler (>=2.4.1)
5. Открыть `firmware/esp32_control.ino`
6. Upload

### PlatformIO (альтернатива)

```bash
pip install platformio
cd esp32-control
pio init --board esp32-c3-devkitc-02
# Добавить в platformio.ini:
# [env:esp32-c3-devkitc-02]
# platform = espressif32
# board = esp32-c3-devkitc-02
# framework = arduino
# lib_deps =
#   adafruit/Adafruit NeoPixel @ ^1.12.0
#   ESP32Async/ESPAsyncWebServer @ ^3.0.0
#   ESP32Async/AsyncTCP @ ^1.1.4
#   madhephaestus/ESP32Servo @ ^3.0.0
#   bblanchon/ArduinoJson @ ^6.21.0
#   links2004/arduinoWebSockets @ ^2.4.1
# build_flags = -DBOARD_HAS_PSRAM
pio run -t upload
```

## 🚀 Использование

1. После прошивки ESP32-C3 создаст WiFi сеть **`ESP32-Control`** (пароль: `12345678`)
2. Подключиться к этой сети с телефона/ноута
3. Открыть браузер: **http://192.168.123.1** (или http://192.168.4.1 — стандартный AP IP)
4. Управлять!

### Сменить SSID/пароль

В `firmware/esp32_control.ino`:
```cpp
const char* AP_SSID = "ESP32-Control";
const char* AP_PASS = "12345678";
```

### Подключение к существующей WiFi сети (STA режим)

Замени блок `WiFi.mode(WIFI_AP)...` в `setup()` на:
```cpp
WiFi.mode(WIFI_STA);
WiFi.begin("YOUR_SSID", "YOUR_PASSWORD");
while (WiFi.status() != WL_CONNECTED) {
  delay(500);
  Serial.print(".");
}
Serial.println();
Serial.print("IP: ");
Serial.println(WiFi.localIP());
```
IP адрес появится в Serial Monitor (115200 baud).

## 🎨 UI

**Тёмная тема** с glassmorphism, неоновыми акцентами, плавными анимациями.
- Слайдеры с glow-эффектом на ползунке
- SVG визуализатор угла серво (стрелка + дуга)
- 9 анимаций LED: OFF, Static, Rainbow, Rainbow Cycle, Chase, Breathing, Theater, Wipe, Scanner
- Color picker с превью
- Auto-reconnect при разрыве WebSocket
- Toast уведомления
- Status pill с пульсирующей точкой подключения

## 🛡️ Безопасность

- ⚠️ **SG90 stall current** — 250mA × 2 = 500mA. Mini560 на 1-2A — ок.
- ⚠️ **WS2812B white full** — 14 × 60mA = 840mA. На максимальной яркости возможны просадки.
- ⚠️ **Не подключай серво при включенном питании** — может дёрнуться и сломать механику.
- ⚠️ **Проверь полярность** Mini560 (IN+/IN-) — при перепутывании сгорит.

## 🐛 Troubleshooting

| Симптом | Решение |
|---|---|
| Не подключается к AP | Проверь пароль (мин 8 символов), перезагрузи ESP |
| Серво дрожит | Конденсатор 470μF на 5V, проверь GND |
| LED не светятся | Проверь daisy chain (DOUT→DIN), общая GND |
| Только первый LED работает | Level shifter 74AHCT125 (3.3V→5V сигнал) |
| ESP перезагружается | Питание слабое — нужен кондёр на 5V |
| WebSocket не работает | Браузер блокирует ws:// на https странице — открой через http:// |
| Серво не двигается | Проверь `servo_enabled=true` (тумблер в UI), угол не 90° если 90° |

## 📐 Структура

```
esp32-control/
├── firmware/
│   └── esp32_control.ino     # Arduino скетч (HTML встроен в PROGMEM)
├── web/
│   └── index.html            # Standalone HTML (для разработки UI)
├── README.md
└── WIRING.md
```

## 📡 Протокол WebSocket

Клиент → ESP (JSON):
```json
{"cmd": "servo", "which": 1, "angle": 90}
{"cmd": "servo_enable", "which": 1, "enabled": true}
{"cmd": "led_animation", "name": "rainbow"}
{"cmd": "led_color", "color": "#a855f7"}
{"cmd": "led_brightness", "value": 80}
{"cmd": "led_speed", "value": 50}
{"cmd": "get_state"}
```

ESP → клиент (JSON):
```json
{
  "type": "state",
  "servo1_angle": 90,
  "servo2_angle": 90,
  "servo1_enabled": true,
  "servo2_enabled": true,
  "animation": "rainbow_cycle",
  "color": "#a855f7",
  "brightness": 80,
  "speed": 50
}
```

## 📜 Лицензия

MIT
