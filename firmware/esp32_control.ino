/*
  esp32_control.ino
  ─────────────────
  Firmware for ESP32-C3: 2x SG90 servos + 14x WS2812B LEDs + WebSocket web UI.

  Hardware:
  - ESP32-C3 (any variant with 4MB flash)
  - 2x SG90 servos on GPIO2, GPIO3 (50Hz PWM)
  - 2x WS2812B rings (7 LEDs each, daisy-chained) on GPIO10 = 14 LEDs total
  - Mini560 12V→5V buck converter for power
  - Common ground between ESP32-C3, Mini560, servos, LEDs

  Architecture:
  - AsyncWebServer on port 80 → serves index.html (embedded in PROGMEM)
  - WebSocket on port 81 → real-time control from browser
  - LEDC peripheral for servo PWM (2 channels, 50Hz, 16-bit)
  - RMT peripheral for WS2812B (via Adafruit_NeoPixel)

  Build with Arduino IDE or PlatformIO:
  - Board: ESP32C3 Dev Module
  - Flash size: 4MB
  - Upload speed: 921600
*/

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ─── Configuration ──────────────────────────────────────────────────────

// WiFi — AP mode (no router needed). Change if you want STA mode.
const char* AP_SSID = "ESP32-Control";
const char* AP_PASS = "12345678";  // min 8 chars

// Pins
const int SERVO1_PIN = 2;
const int SERVO2_PIN = 3;
const int LED_PIN    = 10;
const int LED_COUNT  = 14;  // 2 rings × 7 LEDs

// Servo PWM
const int SERVO_MIN_US = 500;    // 0°
const int SERVO_MAX_US = 2400;   // 180°
const int SERVO_FREQ   = 50;     // 50 Hz
const int SERVO_RES    = 16;     // 16-bit resolution

// ─── Globals ────────────────────────────────────────────────────────────

AsyncWebServer server(80);
WebSocketsServer webSocket(81);

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Servo servo1;
Servo servo2;

// State
int servo1Angle = 90;
int servo2Angle = 90;
bool servo1Enabled = true;
bool servo2Enabled = true;

// LED animation state
String currentAnimation = "static";
uint32_t staticColor = 0xFF00FF;  // magenta default
int ledBrightness = 80;           // 0-255
int animationSpeed = 50;          // ms delay between frames
unsigned long lastAnimationUpdate = 0;
int animationStep = 0;

// ─── Embedded web page (PROGMEM) ────────────────────────────────────────

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0">
<title>ESP32 Control Hub</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  :root {
    --bg-0: #060608;
    --bg-1: #0d0d12;
    --bg-2: #15151c;
    --glass: rgba(255,255,255,0.04);
    --glass-bd: rgba(255,255,255,0.08);
    --text: #e8e8f0;
    --text-dim: #8888a0;
    --accent: #a855f7;
    --accent-2: #ec4899;
    --accent-3: #06b6d4;
    --success: #10b981;
    --warn: #f59e0b;
    --danger: #ef4444;
    --radius: 16px;
  }
  html, body {
    min-height: 100vh;
    background: var(--bg-0);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, 'SF Pro Display', 'Segoe UI', system-ui, sans-serif;
    overflow-x: hidden;
  }
  body {
    background:
      radial-gradient(circle at 0% 0%, rgba(168,85,247,0.18), transparent 40%),
      radial-gradient(circle at 100% 0%, rgba(236,72,153,0.15), transparent 40%),
      radial-gradient(circle at 50% 100%, rgba(6,182,212,0.12), transparent 50%),
      var(--bg-0);
    min-height: 100vh;
    padding: 16px;
    padding-bottom: 80px;
  }

  /* Animated background orbs */
  .orbs { position: fixed; inset: 0; z-index: -1; pointer-events: none; overflow: hidden; }
  .orb { position: absolute; border-radius: 50%; filter: blur(80px); opacity: 0.35; animation: float 20s ease-in-out infinite; }
  .orb:nth-child(1) { width: 400px; height: 400px; background: #a855f7; top: -100px; left: -100px; }
  .orb:nth-child(2) { width: 350px; height: 350px; background: #ec4899; top: 30%; right: -120px; animation-delay: -5s; }
  .orb:nth-child(3) { width: 300px; height: 300px; background: #06b6d4; bottom: -80px; left: 30%; animation-delay: -10s; }
  @keyframes float {
    0%, 100% { transform: translate(0,0) scale(1); }
    33% { transform: translate(30px, -50px) scale(1.1); }
    66% { transform: translate(-20px, 30px) scale(0.9); }
  }

  /* Header */
  header {
    text-align: center;
    margin-bottom: 24px;
    padding-top: 16px;
  }
  header h1 {
    font-size: clamp(1.6rem, 4vw, 2.2rem);
    font-weight: 800;
    background: linear-gradient(135deg, #a855f7 0%, #ec4899 50%, #06b6d4 100%);
    -webkit-background-clip: text;
    background-clip: text;
    -webkit-text-fill-color: transparent;
    letter-spacing: -0.02em;
    margin-bottom: 6px;
  }
  header p {
    color: var(--text-dim);
    font-size: 0.85rem;
    font-weight: 500;
  }

  /* Status pill */
  .status-pill {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 6px 14px;
    background: var(--glass);
    border: 1px solid var(--glass-bd);
    border-radius: 99px;
    font-size: 0.78rem;
    margin-top: 10px;
    backdrop-filter: blur(10px);
  }
  .status-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--danger); transition: background 0.3s; }
  .status-dot.connected { background: var(--success); box-shadow: 0 0 12px var(--success); animation: pulse 2s ease-in-out infinite; }
  @keyframes pulse { 50% { transform: scale(1.3); } }

  /* Grid layout */
  .grid {
    display: grid;
    grid-template-columns: 1fr;
    gap: 16px;
    max-width: 1100px;
    margin: 0 auto;
  }
  @media (min-width: 768px) {
    .grid { grid-template-columns: 1fr 1fr; }
    .grid .led-card { grid-column: 1 / -1; }
  }

  /* Glass card */
  .card {
    background: var(--glass);
    border: 1px solid var(--glass-bd);
    border-radius: var(--radius);
    padding: 20px;
    backdrop-filter: blur(20px);
    -webkit-backdrop-filter: blur(20px);
    box-shadow: 0 8px 32px rgba(0,0,0,0.3);
    transition: transform 0.2s, border-color 0.2s;
  }
  .card:hover { border-color: rgba(168,85,247,0.3); }

  .card-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 18px;
  }
  .card-title {
    font-size: 0.95rem;
    font-weight: 700;
    color: var(--text);
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .card-title .icon { font-size: 1.2rem; }
  .card-subtitle { font-size: 0.75rem; color: var(--text-dim); }

  /* Toggle switch */
  .switch {
    position: relative;
    width: 44px;
    height: 24px;
    background: rgba(255,255,255,0.1);
    border-radius: 99px;
    cursor: pointer;
    transition: background 0.25s;
    border: 1px solid var(--glass-bd);
  }
  .switch::after {
    content: '';
    position: absolute;
    top: 2px; left: 2px;
    width: 18px; height: 18px;
    background: #fff;
    border-radius: 50%;
    transition: transform 0.25s cubic-bezier(0.4, 0, 0.2, 1);
  }
  .switch.on { background: linear-gradient(135deg, #a855f7, #ec4899); box-shadow: 0 0 16px rgba(168,85,247,0.5); }
  .switch.on::after { transform: translateX(20px); }

  /* Slider */
  .slider-row { margin-top: 12px; }
  .slider-label {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 8px;
    font-size: 0.82rem;
  }
  .slider-label .value {
    font-weight: 700;
    color: var(--accent);
    font-variant-numeric: tabular-nums;
    background: rgba(168,85,247,0.1);
    padding: 2px 8px;
    border-radius: 6px;
    min-width: 50px;
    text-align: center;
  }
  .slider-label .value.disabled { color: var(--text-dim); background: rgba(255,255,255,0.05); }

  input[type="range"] {
    -webkit-appearance: none;
    width: 100%;
    height: 6px;
    background: rgba(255,255,255,0.08);
    border-radius: 99px;
    outline: none;
    cursor: pointer;
  }
  input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 22px;
    height: 22px;
    border-radius: 50%;
    background: linear-gradient(135deg, #a855f7, #ec4899);
    cursor: pointer;
    box-shadow: 0 0 16px rgba(168,85,247,0.6);
    transition: transform 0.15s;
    border: 2px solid #fff;
  }
  input[type="range"]::-webkit-slider-thumb:hover { transform: scale(1.15); }
  input[type="range"]::-webkit-slider-thumb:active { transform: scale(0.95); }
  input[type="range"]:disabled::-webkit-slider-thumb { background: #555; box-shadow: none; cursor: not-allowed; }
  input[type="range"]::-moz-range-thumb {
    width: 22px; height: 22px; border-radius: 50%;
    background: linear-gradient(135deg, #a855f7, #ec4899);
    border: 2px solid #fff;
    cursor: pointer;
    box-shadow: 0 0 16px rgba(168,85,247,0.6);
  }

  /* Preset buttons */
  .presets {
    display: grid;
    grid-template-columns: repeat(5, 1fr);
    gap: 6px;
    margin-top: 14px;
  }
  .preset {
    background: rgba(255,255,255,0.05);
    border: 1px solid var(--glass-bd);
    color: var(--text);
    padding: 8px 4px;
    border-radius: 8px;
    font-size: 0.78rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s;
  }
  .preset:hover { background: rgba(168,85,247,0.15); border-color: rgba(168,85,247,0.4); }
  .preset:active { transform: scale(0.95); }

  /* Servo visualizer */
  .servo-viz {
    width: 110px;
    height: 110px;
    margin: 8px auto 16px;
    position: relative;
  }
  .servo-viz svg { width: 100%; height: 100%; }
  .servo-viz .dial { fill: none; stroke: rgba(255,255,255,0.06); stroke-width: 8; }
  .servo-viz .arc { fill: none; stroke: url(#grad); stroke-width: 8; stroke-linecap: round; transition: stroke-dasharray 0.3s; }
  .servo-viz .needle { stroke: #fff; stroke-width: 3; stroke-linecap: round; transform-origin: center; transition: transform 0.3s cubic-bezier(0.4,0,0.2,1); filter: drop-shadow(0 0 4px rgba(168,85,247,0.8)); }
  .servo-viz .center-dot { fill: #fff; }
  .servo-viz .label { fill: var(--text-dim); font-size: 11px; font-weight: 600; }

  /* LED card */
  .led-card { grid-column: 1 / -1; }
  .led-grid {
    display: grid;
    grid-template-columns: 1fr;
    gap: 20px;
  }
  @media (min-width: 768px) {
    .led-grid { grid-template-columns: 1fr 1fr; }
  }

  /* Color picker */
  .color-row { display: flex; align-items: center; gap: 12px; margin-bottom: 14px; }
  .color-row label { font-size: 0.82rem; color: var(--text-dim); flex: 1; }
  .color-picker-wrap {
    position: relative;
    width: 56px; height: 32px;
    border-radius: 8px;
    overflow: hidden;
    border: 1px solid var(--glass-bd);
    cursor: pointer;
  }
  .color-picker-wrap input[type="color"] {
    position: absolute;
    inset: -4px;
    width: calc(100% + 8px); height: calc(100% + 8px);
    border: none;
    cursor: pointer;
  }

  /* Animation buttons */
  .anim-grid {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 8px;
    margin-top: 12px;
  }
  @media (min-width: 480px) { .anim-grid { grid-template-columns: repeat(4, 1fr); } }
  @media (min-width: 768px) { .anim-grid { grid-template-columns: repeat(5, 1fr); } }

  .anim-btn {
    aspect-ratio: 1;
    background: rgba(255,255,255,0.04);
    border: 1px solid var(--glass-bd);
    color: var(--text);
    border-radius: 12px;
    font-size: 0.7rem;
    font-weight: 600;
    cursor: pointer;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 4px;
    padding: 6px;
    transition: all 0.15s;
  }
  .anim-btn .ico { font-size: 1.4rem; line-height: 1; }
  .anim-btn:hover { background: rgba(168,85,247,0.12); border-color: rgba(168,85,247,0.3); transform: translateY(-2px); }
  .anim-btn:active { transform: translateY(0); }
  .anim-btn.active {
    background: linear-gradient(135deg, rgba(168,85,247,0.3), rgba(236,72,153,0.3));
    border-color: var(--accent);
    box-shadow: 0 0 20px rgba(168,85,247,0.4);
  }
  .anim-btn.off { color: var(--danger); }
  .anim-btn.off.active { background: rgba(239,68,68,0.2); border-color: var(--danger); box-shadow: 0 0 20px rgba(239,68,68,0.3); }

  /* Toast */
  .toast {
    position: fixed;
    bottom: 24px;
    left: 50%;
    transform: translateX(-50%) translateY(100px);
    background: var(--bg-2);
    border: 1px solid var(--glass-bd);
    color: var(--text);
    padding: 12px 20px;
    border-radius: 12px;
    font-size: 0.85rem;
    backdrop-filter: blur(20px);
    box-shadow: 0 8px 32px rgba(0,0,0,0.5);
    z-index: 1000;
    transition: transform 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    pointer-events: none;
  }
  .toast.show { transform: translateX(-50%) translateY(0); }

  /* Connection overlay */
  .overlay {
    position: fixed;
    inset: 0;
    background: rgba(6,6,8,0.85);
    backdrop-filter: blur(8px);
    z-index: 999;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-direction: column;
    gap: 16px;
    opacity: 0;
    pointer-events: none;
    transition: opacity 0.3s;
  }
  .overlay.show { opacity: 1; pointer-events: auto; }
  .spinner {
    width: 50px; height: 50px;
    border: 3px solid rgba(168,85,247,0.2);
    border-top-color: var(--accent);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }

  /* Mini info row */
  .info-row {
    display: flex;
    justify-content: space-between;
    padding: 6px 0;
    font-size: 0.78rem;
    color: var(--text-dim);
    border-bottom: 1px solid rgba(255,255,255,0.04);
  }
  .info-row:last-child { border-bottom: none; }
  .info-row span:last-child { color: var(--text); font-weight: 600; }
</style>
</head>
<body>

<div class="orbs">
  <div class="orb"></div>
  <div class="orb"></div>
  <div class="orb"></div>
</div>

<header>
  <h1>ESP32 Control Hub</h1>
  <p>Servos · WS2812B · WebSocket · Real-time</p>
  <div class="status-pill">
    <div class="status-dot" id="status-dot"></div>
    <span id="status-text">Connecting…</span>
  </div>
</header>

<div class="grid">

  <!-- Servo 1 -->
  <div class="card">
    <div class="card-header">
      <div>
        <div class="card-title"><span class="icon">⚙️</span> Servo 1</div>
        <div class="card-subtitle">GPIO2 · SG90</div>
      </div>
      <div class="switch on" id="sw-servo1"></div>
    </div>

    <div class="servo-viz">
      <svg viewBox="0 0 100 100">
        <defs>
          <linearGradient id="grad" x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stop-color="#a855f7"/>
            <stop offset="100%" stop-color="#ec4899"/>
          </linearGradient>
        </defs>
        <circle class="dial" cx="50" cy="50" r="38"/>
        <path class="arc" id="arc1" d="M 22 78 A 38 38 0 1 1 78 78" stroke-dasharray="0 200"/>
        <line class="needle" id="needle1" x1="50" y1="50" x2="50" y2="18"/>
        <circle class="center-dot" cx="50" cy="50" r="4"/>
        <text class="label" x="50" y="92" text-anchor="middle">0° ─ 180°</text>
      </svg>
    </div>

    <div class="slider-row">
      <div class="slider-label">
        <span>Угол</span>
        <span class="value" id="val-servo1">90°</span>
      </div>
      <input type="range" id="slider-servo1" min="0" max="180" value="90">
    </div>

    <div class="presets">
      <button class="preset" data-servo="1" data-angle="0">0°</button>
      <button class="preset" data-servo="1" data-angle="45">45°</button>
      <button class="preset" data-servo="1" data-angle="90">90°</button>
      <button class="preset" data-servo="1" data-angle="135">135°</button>
      <button class="preset" data-servo="1" data-angle="180">180°</button>
    </div>
  </div>

  <!-- Servo 2 -->
  <div class="card">
    <div class="card-header">
      <div>
        <div class="card-title"><span class="icon">⚙️</span> Servo 2</div>
        <div class="card-subtitle">GPIO3 · SG90</div>
      </div>
      <div class="switch on" id="sw-servo2"></div>
    </div>

    <div class="servo-viz">
      <svg viewBox="0 0 100 100">
        <circle class="dial" cx="50" cy="50" r="38"/>
        <path class="arc" id="arc2" d="M 22 78 A 38 38 0 1 1 78 78" stroke-dasharray="0 200"/>
        <line class="needle" id="needle2" x1="50" y1="50" x2="50" y2="18"/>
        <circle class="center-dot" cx="50" cy="50" r="4"/>
        <text class="label" x="50" y="92" text-anchor="middle">0° ─ 180°</text>
      </svg>
    </div>

    <div class="slider-row">
      <div class="slider-label">
        <span>Угол</span>
        <span class="value" id="val-servo2">90°</span>
      </div>
      <input type="range" id="slider-servo2" min="0" max="180" value="90">
    </div>

    <div class="presets">
      <button class="preset" data-servo="2" data-angle="0">0°</button>
      <button class="preset" data-servo="2" data-angle="45">45°</button>
      <button class="preset" data-servo="2" data-angle="90">90°</button>
      <button class="preset" data-servo="2" data-angle="135">135°</button>
      <button class="preset" data-servo="2" data-angle="180">180°</button>
    </div>
  </div>

  <!-- LED Control -->
  <div class="card led-card">
    <div class="card-header">
      <div>
        <div class="card-title"><span class="icon">🌈</span> LED Rings</div>
        <div class="card-subtitle">GPIO10 · 2× WS2812B (14 LEDs total)</div>
      </div>
    </div>

    <div class="led-grid">
      <div>
        <div class="color-row">
          <label>Цвет</label>
          <div class="color-picker-wrap">
            <input type="color" id="color-picker" value="#a855f7">
          </div>
        </div>

        <div class="slider-row">
          <div class="slider-label">
            <span>Яркость</span>
            <span class="value" id="val-brightness">80</span>
          </div>
          <input type="range" id="slider-brightness" min="0" max="255" value="80">
        </div>

        <div class="slider-row">
          <div class="slider-label">
            <span>Скорость анимации</span>
            <span class="value" id="val-speed">50ms</span>
          </div>
          <input type="range" id="slider-speed" min="10" max="500" value="50">
        </div>
      </div>

      <div>
        <div class="slider-label" style="margin-bottom: 4px;">
          <span style="font-weight:700;">Анимации</span>
        </div>
        <div class="anim-grid">
          <button class="anim-btn off" data-anim="off"><span class="ico">⭕</span>OFF</button>
          <button class="anim-btn" data-anim="static"><span class="ico">💡</span>Static</button>
          <button class="anim-btn" data-anim="rainbow"><span class="ico">🌈</span>Rainbow</button>
          <button class="anim-btn" data-anim="rainbow_cycle"><span class="ico">🎡</span>Rainbow Cycle</button>
          <button class="anim-btn" data-anim="chase"><span class="ico">➡️</span>Chase</button>
          <button class="anim-btn" data-anim="breathing"><span class="ico">💓</span>Breathing</button>
          <button class="anim-btn" data-anim="theater"><span class="ico">🎭</span>Theater</button>
          <button class="anim-btn" data-anim="wipe"><span class="ico">🧹</span>Wipe</button>
          <button class="anim-btn" data-anim="scanner"><span class="ico">📡</span>Scanner</button>
        </div>
      </div>
    </div>
  </div>

  <!-- System info -->
  <div class="card led-card">
    <div class="card-header">
      <div class="card-title"><span class="icon">📊</span> System</div>
    </div>
    <div class="info-row"><span>WiFi SSID</span><span id="info-ssid">ESP32-Control</span></div>
    <div class="info-row"><span>WebSocket</span><span id="info-ws">—</span></div>
    <div class="info-row"><span>Uptime</span><span id="info-uptime">—</span></div>
    <div class="info-row"><span>LEDs</span><span>14 (2×7)</span></div>
    <div class="info-row"><span>Servos</span><span>2× SG90</span></div>
  </div>

</div>

<div class="toast" id="toast"></div>

<div class="overlay" id="overlay">
  <div class="spinner"></div>
  <div style="color:var(--text-dim);font-size:0.85rem;">Reconnecting…</div>
</div>

<script>
  // ─── WebSocket ──────────────────────────────────────────────────────
  let ws = null;
  let reconnectTimer = null;
  let connected = false;
  const startTime = Date.now();

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = `${proto}//${location.hostname}:81/`;
    console.log('Connecting to', url);
    ws = new WebSocket(url);

    ws.onopen = () => {
      connected = true;
      document.getElementById('status-dot').classList.add('connected');
      document.getElementById('status-text').textContent = 'Connected';
      document.getElementById('overlay').classList.remove('show');
      document.getElementById('info-ws').textContent = 'connected';
      sendCmd({ cmd: 'get_state' });
      showToast('✓ Подключено');
    };

    ws.onclose = () => {
      connected = false;
      document.getElementById('status-dot').classList.remove('connected');
      document.getElementById('status-text').textContent = 'Disconnected';
      document.getElementById('overlay').classList.add('show');
      document.getElementById('info-ws').textContent = 'disconnected';
      reconnectTimer = setTimeout(connect, 2000);
    };

    ws.onerror = () => { ws.close(); };

    ws.onmessage = (e) => {
      try {
        const msg = JSON.parse(e.data);
        if (msg.type === 'state') applyState(msg);
      } catch (err) {
        console.error('Parse error:', err);
      }
    };
  }

  function sendCmd(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  }

  // ─── Apply state from ESP32 ─────────────────────────────────────────
  function applyState(s) {
    if (s.servo1_angle !== undefined) {
      document.getElementById('slider-servo1').value = s.servo1_angle;
      document.getElementById('val-servo1').textContent = s.servo1_angle + '°';
      updateServoViz(1, s.servo1_angle);
    }
    if (s.servo2_angle !== undefined) {
      document.getElementById('slider-servo2').value = s.servo2_angle;
      document.getElementById('val-servo2').textContent = s.servo2_angle + '°';
      updateServoViz(2, s.servo2_angle);
    }
    if (s.servo1_enabled !== undefined) {
      toggleSwitch('sw-servo1', s.servo1_enabled, false);
      document.getElementById('slider-servo1').disabled = !s.servo1_enabled;
      document.getElementById('val-servo1').classList.toggle('disabled', !s.servo1_enabled);
    }
    if (s.servo2_enabled !== undefined) {
      toggleSwitch('sw-servo2', s.servo2_enabled, false);
      document.getElementById('slider-servo2').disabled = !s.servo2_enabled;
      document.getElementById('val-servo2').classList.toggle('disabled', !s.servo2_enabled);
    }
    if (s.animation !== undefined) {
      document.querySelectorAll('.anim-btn').forEach(b => {
        b.classList.toggle('active', b.dataset.anim === s.animation);
      });
    }
    if (s.color !== undefined) {
      document.getElementById('color-picker').value = s.color;
    }
    if (s.brightness !== undefined) {
      document.getElementById('slider-brightness').value = s.brightness;
      document.getElementById('val-brightness').textContent = s.brightness;
    }
    if (s.speed !== undefined) {
      document.getElementById('slider-speed').value = s.speed;
      document.getElementById('val-speed').textContent = s.speed + 'ms';
    }
  }

  // ─── Servo viz ──────────────────────────────────────────────────────
  function updateServoViz(which, angle) {
    const needle = document.getElementById('needle' + which);
    const arc = document.getElementById('arc' + which);
    // 0° = up, 180° = up the other side. Map: -90° (left) → +90° (right)
    const rotation = angle - 90;
    needle.style.transform = `rotate(${rotation}deg)`;
    // Arc fills from -90° to (angle-90)°
    const totalArc = 200; // approx circumference of half circle
    const filled = (angle / 180) * totalArc;
    arc.style.strokeDasharray = `${filled} ${totalArc}`;
  }

  // ─── Toggle switch ──────────────────────────────────────────────────
  function toggleSwitch(id, on, sendUpdate = true) {
    const el = document.getElementById(id);
    el.classList.toggle('on', on);
    if (sendUpdate) {
      const which = id === 'sw-servo1' ? 1 : 2;
      sendCmd({ cmd: 'servo_enable', which, enabled: on });
      document.getElementById('slider-servo' + which).disabled = !on;
      document.getElementById('val-servo' + which).classList.toggle('disabled', !on);
      showToast(`Servo ${which}: ${on ? 'ON' : 'OFF'}`);
    }
  }

  // ─── Event handlers ─────────────────────────────────────────────────
  document.getElementById('sw-servo1').addEventListener('click', () => {
    toggleSwitch('sw-servo1', !document.getElementById('sw-servo1').classList.contains('on'));
  });
  document.getElementById('sw-servo2').addEventListener('click', () => {
    toggleSwitch('sw-servo2', !document.getElementById('sw-servo2').classList.contains('on'));
  });

  // Slider for servos (throttle sending)
  let servoThrottle = {};
  ['slider-servo1', 'slider-servo2'].forEach(id => {
    const slider = document.getElementById(id);
    const which = id === 'slider-servo1' ? 1 : 2;
    slider.addEventListener('input', () => {
      const angle = parseInt(slider.value);
      document.getElementById('val-servo' + which).textContent = angle + '°';
      updateServoViz(which, angle);
      // Throttle: send every 60ms
      if (!servoThrottle[which] || Date.now() - servoThrottle[which] > 60) {
        sendCmd({ cmd: 'servo', which, angle });
        servoThrottle[which] = Date.now();
      }
    });
    slider.addEventListener('change', () => {
      const angle = parseInt(slider.value);
      sendCmd({ cmd: 'servo', which, angle });
    });
  });

  // Preset buttons
  document.querySelectorAll('.preset').forEach(btn => {
    btn.addEventListener('click', () => {
      const which = parseInt(btn.dataset.servo);
      const angle = parseInt(btn.dataset.angle);
      document.getElementById('slider-servo' + which).value = angle;
      document.getElementById('val-servo' + which).textContent = angle + '°';
      updateServoViz(which, angle);
      sendCmd({ cmd: 'servo', which, angle });
    });
  });

  // Color picker
  let colorThrottle = 0;
  document.getElementById('color-picker').addEventListener('input', (e) => {
    if (Date.now() - colorThrottle > 100) {
      sendCmd({ cmd: 'led_color', color: e.target.value });
      colorThrottle = Date.now();
    }
  });

  // Brightness
  document.getElementById('slider-brightness').addEventListener('input', (e) => {
    const v = parseInt(e.target.value);
    document.getElementById('val-brightness').textContent = v;
    sendCmd({ cmd: 'led_brightness', value: v });
  });

  // Speed
  document.getElementById('slider-speed').addEventListener('input', (e) => {
    const v = parseInt(e.target.value);
    document.getElementById('val-speed').textContent = v + 'ms';
    sendCmd({ cmd: 'led_speed', value: v });
  });

  // Animation buttons
  document.querySelectorAll('.anim-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const anim = btn.dataset.anim;
      document.querySelectorAll('.anim-btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      sendCmd({ cmd: 'led_animation', name: anim });
      showToast(`Анимация: ${btn.textContent.trim()}`);
    });
  });

  // ─── Helpers ────────────────────────────────────────────────────────
  function showToast(msg) {
    const toast = document.getElementById('toast');
    toast.textContent = msg;
    toast.classList.add('show');
    clearTimeout(showToast._t);
    showToast._t = setTimeout(() => toast.classList.remove('show'), 1500);
  }

  // Uptime
  setInterval(() => {
    const sec = Math.floor((Date.now() - startTime) / 1000);
    const m = Math.floor(sec / 60);
    const s = sec % 60;
    document.getElementById('info-uptime').textContent = `${m}m ${s}s`;
  }, 1000);

  // Initial state
  updateServoViz(1, 90);
  updateServoViz(2, 90);
  document.querySelector('.anim-btn[data-anim="rainbow_cycle"]').classList.add('active');

  // Connect
  connect();
</script>
</body>
</html>

)HTML";

// ─── Servo helpers ──────────────────────────────────────────────────────

void attachServos() {
  if (servo1Enabled) {
    servo1.attach(SERVO1_PIN, SERVO_MIN_US, SERVO_MAX_US);
    servo1.write(servo1Angle);
  }
  if (servo2Enabled) {
    servo2.attach(SERVO2_PIN, SERVO_MIN_US, SERVO_MAX_US);
    servo2.write(servo2Angle);
  }
}

void detachServos() {
  servo1.detach();
  servo2.detach();
}

void setServo(int which, int angle) {
  angle = constrain(angle, 0, 180);
  if (which == 1) {
    servo1Angle = angle;
    if (servo1Enabled) servo1.write(angle);
  } else {
    servo2Angle = angle;
    if (servo2Enabled) servo2.write(angle);
  }
}

void enableServo(int which, bool enabled) {
  if (which == 1) {
    servo1Enabled = enabled;
    if (enabled) {
      servo1.attach(SERVO1_PIN, SERVO_MIN_US, SERVO_MAX_US);
      servo1.write(servo1Angle);
    } else {
      servo1.detach();
    }
  } else {
    servo2Enabled = enabled;
    if (enabled) {
      servo2.attach(SERVO2_PIN, SERVO_MIN_US, SERVO_MAX_US);
      servo2.write(servo2Angle);
    } else {
      servo2.detach();
    }
  }
}

// ─── LED animations ─────────────────────────────────────────────────────

uint32_t colorWheel(byte wheelPos) {
  wheelPos = 255 - wheelPos;
  if (wheelPos < 85) {
    return strip.Color(255 - wheelPos * 3, 0, wheelPos * 3);
  }
  if (wheelPos < 170) {
    wheelPos -= 85;
    return strip.Color(0, wheelPos * 3, 255 - wheelPos * 3);
  }
  wheelPos -= 170;
  return strip.Color(wheelPos * 3, 255 - wheelPos * 3, 0);
}

void animationStatic() {
  strip.fill(staticColor);
  strip.show();
}

void animationOff() {
  strip.clear();
  strip.show();
}

void animationRainbow() {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, colorWheel((animationStep + i * 256 / LED_COUNT) & 255));
  }
  strip.show();
}

void animationChase() {
  strip.clear();
  for (int i = 0; i < 3; i++) {
    int pos = (animationStep + i) % LED_COUNT;
    strip.setPixelColor(pos, staticColor);
  }
  strip.show();
}

void animationBreathing() {
  int brightness = (sin(animationStep * 0.1) + 1.0) * 0.5 * 255;
  uint8_t r = (staticColor >> 16) & 0xFF;
  uint8_t g = (staticColor >> 8) & 0xFF;
  uint8_t b = staticColor & 0xFF;
  r = r * brightness / 255;
  g = g * brightness / 255;
  b = b * brightness / 255;
  strip.fill(strip.Color(r, g, b));
  strip.show();
}

void animationRainbowCycle() {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, colorWheel(((animationStep * 5) + i * 256 / LED_COUNT) & 255));
  }
  strip.show();
}

void animationTheaterChase() {
  strip.clear();
  for (int i = animationStep % 3; i < LED_COUNT; i += 3) {
    strip.setPixelColor(i, staticColor);
  }
  strip.show();
}

void animationColorWipe() {
  strip.clear();
  int fillCount = (animationStep % (LED_COUNT * 2));
  if (fillCount < LED_COUNT) {
    for (int i = 0; i <= fillCount; i++) {
      strip.setPixelColor(i, staticColor);
    }
  } else {
    int clearCount = fillCount - LED_COUNT;
    for (int i = 0; i < LED_COUNT - clearCount - 1; i++) {
      strip.setPixelColor(i, staticColor);
    }
  }
  strip.show();
}

void animationScanner() {
  strip.clear();
  int pos = (sin(animationStep * 0.15) + 1.0) * 0.5 * (LED_COUNT - 1);
  strip.setPixelColor((int)pos, staticColor);
  if (pos > 0) strip.setPixelColor((int)pos - 1, strip.Color(
    ((staticColor >> 16) & 0xFF) / 3,
    ((staticColor >> 8) & 0xFF) / 3,
    (staticColor & 0xFF) / 3));
  if (pos < LED_COUNT - 1) strip.setPixelColor((int)pos + 1, strip.Color(
    ((staticColor >> 16) & 0xFF) / 3,
    ((staticColor >> 8) & 0xFF) / 3,
    (staticColor & 0xFF) / 3));
  strip.show();
}

void updateAnimation() {
  if (millis() - lastAnimationUpdate < (unsigned long)animationSpeed) return;
  lastAnimationUpdate = millis();
  animationStep++;

  strip.setBrightness(ledBrightness);

  if (currentAnimation == "off")              animationOff();
  else if (currentAnimation == "static")      animationStatic();
  else if (currentAnimation == "rainbow")     animationRainbow();
  else if (currentAnimation == "rainbow_cycle") animationRainbowCycle();
  else if (currentAnimation == "chase")       animationChase();
  else if (currentAnimation == "breathing")   animationBreathing();
  else if (currentAnimation == "theater")     animationTheaterChase();
  else if (currentAnimation == "wipe")        animationColorWipe();
  else if (currentAnimation == "scanner")     animationScanner();
  else                                        animationOff();
}

// ─── WebSocket handlers ─────────────────────────────────────────────────

void sendState(uint8_t clientNum) {
  StaticJsonDocument<512> doc;
  doc["type"] = "state";
  doc["servo1_angle"] = servo1Angle;
  doc["servo2_angle"] = servo2Angle;
  doc["servo1_enabled"] = servo1Enabled;
  doc["servo2_enabled"] = servo2Enabled;
  doc["animation"] = currentAnimation;
  doc["color"] = String("#") + String(staticColor, HEX);
  doc["brightness"] = ledBrightness;
  doc["speed"] = animationSpeed;
  String response;
  serializeJson(doc, response);
  webSocket.sendTXT(clientNum, response);
}

void broadcastState() {
  StaticJsonDocument<512> doc;
  doc["type"] = "state";
  doc["servo1_angle"] = servo1Angle;
  doc["servo2_angle"] = servo2Angle;
  doc["servo1_enabled"] = servo1Enabled;
  doc["servo2_enabled"] = servo2Enabled;
  doc["animation"] = currentAnimation;
  doc["color"] = String("#") + String(staticColor, HEX);
  doc["brightness"] = ledBrightness;
  doc["speed"] = animationSpeed;
  String response;
  serializeJson(doc, response);
  webSocket.broadcastTXT(response);
}

void handleWebSocketMessage(uint8_t clientNum, const char* payload) {
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print(F("JSON parse failed: "));
    Serial.println(error.c_str());
    return;
  }

  String cmd = doc["cmd"] | "";

  if (cmd == "get_state") {
    sendState(clientNum);
    return;
  }
  if (cmd == "servo") {
    int which = doc["which"] | 0;
    int angle = doc["angle"] | -1;
    if (which > 0 && angle >= 0) {
      setServo(which, angle);
    }
    return;
  }
  if (cmd == "servo_enable") {
    int which = doc["which"] | 0;
    bool enabled = doc["enabled"] | false;
    if (which > 0) {
      enableServo(which, enabled);
    }
    return;
  }
  if (cmd == "led_animation") {
    currentAnimation = doc["name"] | "off";
    animationStep = 0;
    return;
  }
  if (cmd == "led_color") {
    String hex = doc["color"] | "#ff00ff";
    long rgb = strtol(hex.c_str() + 1, nullptr, 16);
    staticColor = strip.Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    return;
  }
  if (cmd == "led_brightness") {
    ledBrightness = constrain((int)doc["value"] | 80, 0, 255);
    return;
  }
  if (cmd == "led_speed") {
    animationSpeed = constrain((int)doc["value"] | 50, 10, 500);
    return;
  }
}

void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected\n", clientNum);
      break;
    case WStype_CONNECTED:
      Serial.printf("[%u] Connected\n", clientNum);
      sendState(clientNum);
      break;
    case WStype_TEXT:
      handleWebSocketMessage(clientNum, (const char*)payload);
      break;
    case WStype_BIN:
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }
}

// ─── Setup ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== ESP32 Control Hub ==="));

  // LEDs init
  strip.begin();
  strip.setBrightness(ledBrightness);
  strip.fill(strip.Color(50, 50, 50));
  strip.show();
  Serial.println(F("LEDs initialized"));

  // Servos init — ESP32Servo allocates LEDC channels
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servo1.setPeriodHertz(SERVO_FREQ);
  servo2.setPeriodHertz(SERVO_FREQ);
  attachServos();
  Serial.println(F("Servos initialized"));

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print(F("AP IP: "));
  Serial.println(WiFi.softAPIP());
  Serial.print(F("SSID: "));
  Serial.println(AP_SSID);

  // HTTP server — serve index.html
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{";
    json += "\"servo1_angle\":" + String(servo1Angle) + ",";
    json += "\"servo2_angle\":" + String(servo2Angle) + ",";
    json += "\"servo1_enabled\":" + String(servo1Enabled ? "true" : "false") + ",";
    json += "\"servo2_enabled\":" + String(servo2Enabled ? "true" : "false") + ",";
    json += "\"animation\":\"" + currentAnimation + "\",";
    json += "\"brightness\":" + String(ledBrightness) + ",";
    json += "\"speed\":" + String(animationSpeed);
    json += "}";
    req->send(200, "application/json", json);
  });
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not Found");
  });
  server.begin();
  Serial.println(F("HTTP server on :80"));

  // WebSocket
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  Serial.println(F("WebSocket on :81"));

  // Boot animation
  currentAnimation = "rainbow_cycle";
  animationSpeed = 30;
  Serial.println(F("Ready!"));
}

// ─── Loop ───────────────────────────────────────────────────────────────

void loop() {
  webSocket.loop();
  updateAnimation();
  // Small yield to keep WiFi happy
  if (animationSpeed > 50) {
    delay(1);
  }
}
