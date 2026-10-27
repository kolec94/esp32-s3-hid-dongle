/*
 * ESP32-S3 USB-HID keyboard + mouse dongle — shared logic for all boards.
 *
 * Plug into the TARGET computer; it enumerates as a real USB keyboard + mouse. At
 * boot it serves a WiFi access point (http://192.168.4.1); the button toggles to a
 * BLE control service (web/control.html). Text/keys/mouse arrive over whichever
 * radio is active.
 *
 * ALL board-specific hardware (pins, screen driver, display) lives in boards/<board>.h,
 * so a change to one board can't affect another. Select the board with -DBOARD=n
 * (1 LilyGo, 2 Geek, 3 AtomS3U, 4 AtomS3U Gamepad) or the #define below.
 *
 * Board options: USB CDC On Boot Enabled, USB Mode USB-OTG (TinyUSB),
 *   Partition 16M (3MB APP) [AtomS3U: 8M], Flash 16MB [AtomS3U: 8MB].
 * Libraries: Adafruit GFX, Adafruit ST7735 and ST7789 (pulled in by the board header).
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"
#include "USBHIDGamepad.h"
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <esp_mac.h>
#include <esp_random.h>

// ===================== BOARD SELECT =====================
#define BOARD_LILYGO_TDONGLE_S3 1
#define BOARD_WAVESHARE_S3_GEEK 2
#define BOARD_M5STACK_ATOMS3U   3
#define BOARD_M5STACK_ATOMS3U_GAMEPAD 4
#ifndef BOARD
#define BOARD BOARD_WAVESHARE_S3_GEEK   // default; CI/arduino-cli pass -DBOARD=n
#endif
#if   BOARD == BOARD_LILYGO_TDONGLE_S3
  #include "boards/lilygo.h"
#elif BOARD == BOARD_WAVESHARE_S3_GEEK
  #include "boards/geek.h"
#elif BOARD == BOARD_M5STACK_ATOMS3U
  #include "boards/atoms3u.h"
#elif BOARD == BOARD_M5STACK_ATOMS3U_GAMEPAD
  #include "boards/atoms3u.h"
#else
  #error "Unknown BOARD"
#endif

// 0 = open BLE control (reliable); 1 = require an on-screen pairing PIN (needs a display)
#define REQUIRE_BLE_PAIRING 0
#if REQUIRE_BLE_PAIRING && !HAS_DISPLAY
  #error "REQUIRE_BLE_PAIRING needs a display to show the PIN; this board has none."
#endif

// RGB565 title colours (so shared code needs no display library)
#define DC_CYAN    0x07FF
#define DC_GREEN   0x07E0
#define DC_MAGENTA 0xF81F

// ---- Nordic UART Service UUIDs (match web/control.html) ----
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
USBHIDGamepad Gamepad;
WebServer server(80);
BLEAdvertising *advertising = nullptr;

String apSsid, apPass;
volatile uint32_t g_pairPin = 0;
volatile bool g_showPin = false;
volatile bool needRedraw = false;
int radioMode = 0;            // 0 = WiFi AP, 1 = BLE (button toggles)
volatile bool bleConnected = false;
volatile int wifiClients = 0;
bool wifiApUp = false;
bool bleAdvOn = false;

struct GamepadState {
  int8_t x = 0, y = 0, z = 0, rz = 0, rx = 0, ry = 0;
  uint8_t hat = HAT_CENTER;
  uint32_t buttons = 0;
};
GamepadState gamepadState;
portMUX_TYPE gamepadMux = portMUX_INITIALIZER_UNLOCKED;
bool gamepadDirty = false;
uint32_t lastGamepadUpdateMs = 0;
constexpr uint32_t GAMEPAD_FAILSAFE_MS = 600;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>S3 Keyboard</title><style>
body{font-family:system-ui,sans-serif;max-width:640px;margin:24px auto;padding:0 16px;background:#111;color:#eee}
h1{font-size:1.2rem}
textarea{width:100%;height:150px;font-size:16px;padding:10px;box-sizing:border-box;background:#1c1c1c;color:#eee;border:1px solid #444;border-radius:8px}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px;align-items:center}
button{font-size:15px;padding:10px 16px;border:0;border-radius:8px;cursor:pointer;background:#3b82f6;color:#fff}
button.alt{background:#374151}label{display:flex;align-items:center;gap:6px;font-size:14px}
#log{margin-top:12px;font-size:13px;color:#9ca3af}
h2{font-size:1rem;color:#9ca3af;margin-top:22px}
#pad{touch-action:none;height:170px;margin-top:10px;background:#1c1c1c;border:1px dashed #555;border-radius:10px;display:flex;align-items:center;justify-content:center;color:#666;user-select:none;font-size:14px}
</style></head><body>
<h1>ESP32-S3 — Remote Keyboard (WiFi)</h1>
<textarea id="txt" placeholder="Type text to send to the target computer..."></textarea>
<div class="row"><button onclick="send()">Send</button>
<label><input type="checkbox" id="enter" checked> Press Enter after</label></div>
<div class="row">
<button class="alt" onclick="key('ENTER')">Enter</button>
<button class="alt" onclick="key('TAB')">Tab</button>
<button class="alt" onclick="key('ESC')">Esc</button>
<button class="alt" onclick="key('WIN')">Win/Cmd</button>
<button class="alt" onclick="key('CTRL_ALT_DEL')">Ctrl+Alt+Del</button></div>
<h2>Mouse</h2>
<div id="pad">drag to move the pointer &#8226; tap to left-click</div>
<div class="row"><label>Sensitivity <input type="range" id="sens" min="0.3" max="5" step="0.1" value="1.6"> <span id="sensv">1.6</span>x</label></div>
<div class="row">
<button class="alt" onclick="mclick(1)">Left</button>
<button class="alt" onclick="mclick(2)">Right</button>
<button class="alt" onclick="mclick(4)">Middle</button>
<button class="alt" onclick="mscroll(-3)">Scroll up</button>
<button class="alt" onclick="mscroll(3)">Scroll down</button></div>
<h2>Gamepad</h2>
<div class="row" id="dpad">
<button class="alt" data-hat="1">↑</button><button class="alt" data-hat="7">←</button>
<button class="alt" data-hat="5">↓</button><button class="alt" data-hat="3">→</button>
</div>
<div class="row" id="buttons">
<button class="alt" data-button="0">A</button><button class="alt" data-button="1">B</button>
<button class="alt" data-button="3">X</button><button class="alt" data-button="4">Y</button>
<button class="alt" data-button="6">LB</button><button class="alt" data-button="7">RB</button>
<button class="alt" data-button="8">LT</button><button class="alt" data-button="9">RT</button>
<button class="alt" data-button="10">Back</button><button class="alt" data-button="11">Start</button>
</div>
<div class="row"><label>Left stick X <input type="range" data-axis="0" min="-127" max="127" value="0"></label>
<label>Y <input type="range" data-axis="1" min="-127" max="127" value="0"></label></div>
<div class="row"><label>Right stick X <input type="range" data-axis="2" min="-127" max="127" value="0"></label>
<label>Y <input type="range" data-axis="3" min="-127" max="127" value="0"></label></div>
<div class="row"><label>L2 axis <input type="range" data-axis="4" min="0" max="127" value="0"></label>
<label>R2 axis <input type="range" data-axis="5" min="0" max="127" value="0"></label></div>
<div id="log"></div><script>
async function post(p,b){const r=await fetch(p,{method:'POST',headers:{'Content-Type':'text/plain'},body:b});
document.getElementById('log').textContent=r.ok?'sent':'error '+r.status;}
function send(){let t=document.getElementById('txt').value;if(document.getElementById('enter').checked)t+='\n';post('/type',t);}
function key(k){post('/key',k);}
let SENS=parseFloat(localStorage.getItem('sens')||'1.6');
const sens=document.getElementById('sens'),sensv=document.getElementById('sensv');
sens.value=SENS;sensv.textContent=SENS.toFixed(1);
sens.addEventListener('input',()=>{SENS=parseFloat(sens.value);sensv.textContent=SENS.toFixed(1);localStorage.setItem('sens',sens.value);});
function clamp(v){v=v|0;return v<-127?-127:v>127?127:v;}
function mpost(a){fetch('/mouse',{method:'POST',body:new Uint8Array(a)});}
function mclick(b){mpost([2,0x63,b]);}
function mscroll(a){mpost([2,0x73,(clamp(a)+128)&255]);}
const pad=document.getElementById('pad');let pd=false,lx=0,ly=0,ax=0,ay=0,mv=false;
pad.addEventListener('pointerdown',e=>{pd=true;lx=e.clientX;ly=e.clientY;mv=false;ax=ay=0;pad.setPointerCapture(e.pointerId);e.preventDefault();});
pad.addEventListener('pointermove',e=>{if(!pd)return;ax+=e.clientX-lx;ay+=e.clientY-ly;lx=e.clientX;ly=e.clientY;if(Math.abs(ax)>3||Math.abs(ay)>3)mv=true;});
pad.addEventListener('pointerup',e=>{if(!pd)return;pd=false;if(!mv)mclick(1);});
setInterval(()=>{if(!ax&&!ay)return;const dx=clamp(Math.round(ax*SENS)),dy=clamp(Math.round(ay*SENS));ax=0;ay=0;if(dx||dy)mpost([2,0x6d,(dx+128)&255,(dy+128)&255]);},40);
const gp={axes:[0,0,0,0,0,0],hat:0,buttons:0};
function gpSend(){const a=[3,0x67,...gp.axes.map(v=>(v+128)&255),gp.hat];
 for(let i=0;i<4;i++)a.push((gp.buttons>>>(8*i))&255);
 fetch('/gamepad',{method:'POST',headers:{'Content-Type':'text/plain'},body:new Uint8Array(a)}).catch(gpClear);}
function gpClear(){gp.axes.fill(0);gp.hat=0;gp.buttons=0;dirs=0;
 document.querySelectorAll('[data-axis]').forEach(el=>el.value=0);}
function held(el,on){const n=Number(el.dataset.button),mask=(1<<n)>>>0;
 el.addEventListener('pointerdown',e=>{e.preventDefault();el.setPointerCapture(e.pointerId);gp.buttons=(gp.buttons|mask)>>>0;gpSend();});
 const up=()=>{gp.buttons=(gp.buttons&~mask)>>>0;gpSend();};
 el.addEventListener('pointerup',up);el.addEventListener('pointercancel',up);el.addEventListener('lostpointercapture',up);}
document.querySelectorAll('[data-button]').forEach(el=>held(el));
let dirs=0;
function sendHat(){const u=dirs&1,d=dirs&2,l=dirs&4,r=dirs&8;
 gp.hat=u?(r?2:(l?8:1)):d?(r?4:(l?6:5)):(r?3:(l?7:0));gpSend();}
document.querySelectorAll('[data-hat]').forEach(el=>{const bit=({1:1,5:2,7:4,3:8})[el.dataset.hat];
 const down=e=>{e.preventDefault();el.setPointerCapture(e.pointerId);dirs|=bit;sendHat();};
 const up=()=>{dirs&=~bit;sendHat();};el.addEventListener('pointerdown',down);el.addEventListener('pointerup',up);el.addEventListener('pointercancel',up);el.addEventListener('lostpointercapture',up);});
document.querySelectorAll('[data-axis]').forEach(el=>el.addEventListener('input',()=>{gp.axes[Number(el.dataset.axis)]=Number(el.value);gpSend();}));
setInterval(()=>{if(gp.buttons||gp.hat||gp.axes.some(v=>v!==0))gpSend();},200);
document.addEventListener('visibilitychange',()=>{if(document.hidden){gpClear();gpSend();}});
</script></body></html>
)HTML";

void doKey(const String &k) {
  if (k == "ENTER")      Keyboard.write(KEY_RETURN);
  else if (k == "TAB")   Keyboard.write(KEY_TAB);
  else if (k == "ESC")   Keyboard.write(KEY_ESC);
  else if (k == "WIN")   Keyboard.write(KEY_LEFT_GUI);
  else if (k == "CTRL_ALT_DEL") {
    Keyboard.press(KEY_LEFT_CTRL); Keyboard.press(KEY_LEFT_ALT); Keyboard.press(KEY_DELETE);
    delay(60); Keyboard.releaseAll();
  }
}

// Mouse command bytes (all non-zero): 0x02 'm' dx+128 dy+128 | 'c' btn | 's' amt+128
void doMouse(const uint8_t *d, size_t n) {
  if (n < 2) return;
  if (d[1] == 'm' && n >= 4)      Mouse.move((int)d[2] - 128, (int)d[3] - 128, 0);
  else if (d[1] == 'c' && n >= 3) Mouse.click(d[2]);
  else if (d[1] == 's' && n >= 3) Mouse.move(0, 0, (int)d[2] - 128);
}

// Gamepad packet: 0x03 'g', six signed axes biased by 128, hat (0=center, 1..8),
// then a little-endian 32-bit button mask. Hold states are refreshed by the
// controller; if refreshes stop, the loop clears the report after 600 ms.
void doGamepad(const uint8_t *d, size_t n) {
  if (n != 13 || d[0] != 0x03 || d[1] != 'g') return;
  GamepadState next;
  next.x = (int8_t)((int)d[2] - 128);
  next.y = (int8_t)((int)d[3] - 128);
  next.z = (int8_t)((int)d[4] - 128);
  next.rz = (int8_t)((int)d[5] - 128);
  next.rx = (int8_t)((int)d[6] - 128);
  next.ry = (int8_t)((int)d[7] - 128);
  next.hat = d[8] <= 8 ? d[8] : HAT_CENTER;
  next.buttons = (uint32_t)d[9] | ((uint32_t)d[10] << 8) |
                 ((uint32_t)d[11] << 16) | ((uint32_t)d[12] << 24);
  portENTER_CRITICAL(&gamepadMux);
  gamepadState = next;
  gamepadDirty = true;
  lastGamepadUpdateMs = millis();
  portEXIT_CRITICAL(&gamepadMux);
}

void releaseGamepad() {
  portENTER_CRITICAL(&gamepadMux);
  gamepadState = GamepadState{};
  gamepadDirty = true;
  lastGamepadUpdateMs = millis();
  portEXIT_CRITICAL(&gamepadMux);
}

void serviceGamepad() {
  GamepadState state;
  bool dirty;
  portENTER_CRITICAL(&gamepadMux);
  const bool active = gamepadState.x || gamepadState.y || gamepadState.z ||
      gamepadState.rz || gamepadState.rx || gamepadState.ry ||
      gamepadState.hat != HAT_CENTER || gamepadState.buttons;
  if (active && millis() - lastGamepadUpdateMs > GAMEPAD_FAILSAFE_MS) {
    gamepadState = GamepadState{};
    gamepadDirty = true;
  }
  state = gamepadState;
  dirty = gamepadDirty;
  gamepadDirty = false;
  portEXIT_CRITICAL(&gamepadMux);
  if (dirty) Gamepad.send(state.x, state.y, state.z, state.rz, state.rx,
                          state.ry, state.hat, state.buttons);
}

// shared screen content -> board renders it (boardRenderScreen lives in boards/<board>.h)
void drawScreen() {
  if (g_showPin) {
    char pin[8]; snprintf(pin, sizeof(pin), "%06u", (unsigned)g_pairPin);
    boardRenderScreen("BLE PAIRING", DC_MAGENTA, "Enter PIN:", String(pin), "");
  } else if (radioMode == 0) {
    boardRenderScreen("WiFi MODE", DC_CYAN, "SSID: " + apSsid, "Pass: " + apPass, "192.168.4.1");
  } else {
    boardRenderScreen("BLE MODE", DC_GREEN, "name: " + apSsid,
                      bleConnected ? "connected" : "advertising", "(btn=WiFi)");
  }
}

void genCreds() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char ss[20];
  snprintf(ss, sizeof(ss), "Keeb-%02X%02X", mac[4], mac[5]);
  apSsid = ss;
#if defined(FIXED_PASS)
  apPass = "type12345";
#elif HAS_DISPLAY
  static const char cs[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // all caps + digits, no I/O/0/1
  apPass = "";
  for (int i = 0; i < 8; i++) apPass += cs[esp_random() % (sizeof(cs) - 1)];
#else
  apPass = "type12345";   // no screen to show a random one
#endif
  Serial.printf("[%s] WiFi: %s / %s  ->  http://192.168.4.1\n", BOARD_NAME, apSsid.c_str(), apPass.c_str());
}

void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  wifiApUp = true;
}
void stopAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiApUp = false;
  wifiClients = 0;
}
void onWiFiEvent(WiFiEvent_t e) {
  if (e == ARDUINO_EVENT_WIFI_AP_STACONNECTED) wifiClients++;
  else if (e == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED && wifiClients > 0) {
    wifiClients--;
    if (wifiClients == 0) releaseGamepad();
  }
}
void startAdv() { if (!bleAdvOn) { advertising->start(); bleAdvOn = true; } }
void stopAdv()  { if (bleAdvOn)  { advertising->stop();  bleAdvOn = false; } }

class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { bleConnected = true; }
  void onDisconnect(BLEServer *s) override { bleConnected = false; releaseGamepad(); }
};
class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    if (g_showPin) { g_showPin = false; needRedraw = true; }
    uint8_t *v = c->getData();
    size_t n = c->getLength();
    if (n == 0) return;
    if (v[0] == 0x01) {
      String key;
      for (size_t i = 1; i < n; i++) key += (char)v[i];
      doKey(key);
    } else if (v[0] == 0x02) doMouse(v, n);
    else if (v[0] == 0x03) doGamepad(v, n);
    else {
      String text;
      for (size_t i = 0; i < n; i++) text += (char)v[i];
      Keyboard.print(text);
    }
  }
};
#if REQUIRE_BLE_PAIRING
class SecCb : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t pk) override { g_pairPin = pk; g_showPin = true; needRedraw = true; }
  bool onConfirmPIN(uint32_t pk) override { return true; }
  bool onSecurityRequest() override { return true; }
};
#endif

void applyRadio() {
  releaseGamepad();
  if (radioMode == 0) { stopAdv(); if (!wifiApUp) startAP(); }
  else                { stopAP();  startAdv(); }
  needRedraw = true;
}
void pollButton() {
  static bool last = HIGH;
  static uint32_t t = 0;
  bool b = digitalRead(BTN_PIN);
  if (b != last && millis() - t > 60) {
    t = millis(); last = b;
    if (b == LOW) { radioMode = !radioMode; applyRadio(); }
  }
}

void setup() {
  Serial.begin(115200);
  Keyboard.begin();
  Mouse.begin();
  Gamepad.begin();
  USB.begin();

  boardInitDisplay();
  genCreds();

  pinMode(BTN_PIN, INPUT_PULLUP);
  WiFi.onEvent(onWiFiEvent);
  startAP();                        // WiFi up before BLE init (coexistence order)

  BLEDevice::init(apSsid.c_str());
  BLEServer *srv = BLEDevice::createServer();
  srv->setCallbacks(new SrvCb());
  BLEService *svc = srv->createService(SERVICE_UUID);
  BLECharacteristic *rx = svc->createCharacteristic(
      CHAR_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
#if REQUIRE_BLE_PAIRING
  rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
#endif
  rx->setCallbacks(new RxCb());
  BLECharacteristic *tx = svc->createCharacteristic(CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  tx->addDescriptor(new BLE2902());
  svc->start();

  advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

#if REQUIRE_BLE_PAIRING
  BLEDevice::setSecurityCallbacks(new SecCb());
  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  sec->setCapability(ESP_IO_CAP_OUT);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
#endif

  server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/type", HTTP_POST, []() { Keyboard.print(server.arg("plain")); server.send(200, "text/plain", "ok"); });
  server.on("/key",  HTTP_POST, []() { doKey(server.arg("plain")); server.send(200, "text/plain", "ok"); });
  server.on("/mouse", HTTP_POST, []() { String b = server.arg("plain"); doMouse((const uint8_t *)b.c_str(), b.length()); server.send(200, "text/plain", "ok"); });
  server.on("/gamepad", HTTP_POST, []() { String b = server.arg("plain"); doGamepad((const uint8_t *)b.c_str(), b.length()); server.send(200, "text/plain", "ok"); });
  server.begin();

  radioMode = 0;
  applyRadio();
  drawScreen();
}

void loop() {
  server.handleClient();
  pollButton();
  serviceGamepad();
  if (needRedraw) { needRedraw = false; drawScreen(); }
  static uint32_t t = 0;
  if (millis() - t > 5000) {
    t = millis();
    Serial.printf("[%s] mode=%s WiFi: %s / %s\n", BOARD_NAME,
                  radioMode == 0 ? "WiFi" : "BLE", apSsid.c_str(), apPass.c_str());
  }
  delay(10);
}
