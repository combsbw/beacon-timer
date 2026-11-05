/*
 * Beacon Timer - interval timer with two RGB LEDs, for the ESP32-S3 Super Mini
 *
 *   Board   : esp32:esp32:esp32s3  (arduino-esp32 core 2.0.17), USB CDC On Boot = enabled
 *   Wi-Fi   : open soft AP "Beacon-timer"  ->  http://192.168.4.1  (captive portal opens the page by itself)
 *   Update  : ArduinoOTA on port 3232 ("make ota") and an HTTP upload from the Settings > Device page
 *
 * The timer runs on the device, so it keeps going with no phone connected. The web page is the control panel:
 * it starts, pauses and stops the timer and edits the settings, which are kept in flash.
 *
 * Default wiring (common anode, the anode pin is driven HIGH; change pins and polarity in Settings > Device):
 *   Timer LED : R = GPIO13, G = GPIO11, B = GPIO10, anode = GPIO12
 *   Set LED   : R = GPIO2,  G = GPIO4,  B = GPIO5,  anode = GPIO3
 * Put a series resistor on each color leg. A GPIO is only good for about 20 mA, so keep the total through one
 * anode pin below that, or switch the anode with a transistor and select "no common pin" in the UI.
 *
 * Files: engine.h (timing engine, also built and tested on a PC), json.h + config.h (settings parser),
 *        webui.h (the web page, gzipped; made from ui/app.html by tools/embed_ui.py).
 */
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "driver/gpio.h"
#include "esp_timer.h"

#include "engine.h"
#include "json.h"
#include "config.h"
#include "webui.h"
#include "icons.h"

#define FW_VERSION "0.2.0"
#define AP_SSID "Beacon-timer"          // open network, no password
#define AP_CHANNEL 6
#define AP_MAX_CLIENTS 4
#define HOST_NAME "beacon-timer"
#define OTA_PORT 3232
#define CAPTIVE_PORTAL 1                // answer every DNS name with 192.168.4.1 so phones open the page automatically
#define CFG_MAX_BYTES 6000
#define PRESETS_MAX_BYTES 32000          // saved timers: one JSON array in LittleFS (the "spiffs" partition of the default scheme)
static bool fsOk = false;

// ---- LED PWM ------------------------------------------------------------------------------------------------------
static const int PWM_FREQ = 5000;
static const int PWM_BITS = 12;
static const int PWM_MAX = (1 << PWM_BITS) - 1;

struct LedHw {
  int8_t pin[3];
  int8_t com;
  bool anode;
  bool live;
};
static LedHw hw[2];

static void hwRelease(LedHw& h) {
  if (!h.live) return;
  for (int i = 0; i < 3; i++) { ledcDetachPin(h.pin[i]); pinMode(h.pin[i], INPUT); }
  if (h.com >= 0) pinMode(h.com, INPUT);
  h.live = false;
}

// (Re)assign one LED's pins. Output stays dark: common anode = pins high, common cathode = pins low.
static void hwApply(int idx, const bc::PinCfg& p) {
  LedHw& h = hw[idx];
  bool same = h.live && h.pin[0] == p.r && h.pin[1] == p.g && h.pin[2] == p.b && h.com == p.com;
  if (!same) {
    hwRelease(h);
    h.pin[0] = p.r; h.pin[1] = p.g; h.pin[2] = p.b; h.com = p.com;
    for (int i = 0; i < 3; i++) {
      int ch = idx * 3 + i;
      ledcSetup(ch, PWM_FREQ, PWM_BITS);
      ledcAttachPin(h.pin[i], ch);
      ledcWrite(ch, p.anode ? PWM_MAX : 0);
    }
    h.live = true;
  }
  h.anode = p.anode;
  if (h.com >= 0) {
    pinMode(h.com, OUTPUT);
    gpio_set_drive_capability((gpio_num_t)h.com, GPIO_DRIVE_CAP_3);     // strongest drive: this pin feeds the whole LED
    digitalWrite(h.com, p.anode ? HIGH : LOW);
  }
}

// color 0..255, brightness 0..1 (both perceptual). Gamma turns them into linear light for the PWM.
static void hwWrite(int idx, const bc::Rgb& c, float b, float master) {
  LedHw& h = hw[idx];
  if (!h.live) return;
  const float v[3] = {c.r, c.g, c.b};
  for (int i = 0; i < 3; i++) {
    float x = bc::clampf(v[i] / 255.f * b * master, 0.f, 1.f);
    int d = (int)(powf(x, bc::GAMMA_F) * PWM_MAX + 0.5f);
    if (h.anode) d = PWM_MAX - d;                 // common anode: the LED lights when the pin is pulled LOW
    ledcWrite(idx * 3 + i, d);
  }
}

// ---- shared state -------------------------------------------------------------------------------------------------
static bc::Engine eng;
static SemaphoreHandle_t engMtx;
static volatile bool otaBusy = false;
static volatile uint32_t rebootAtMs = 0;

static Preferences prefs;
static String cfgRaw;                 // the settings JSON exactly as the UI sent it
static uint32_t cfgRev = 1;
static bool cfgDirty = false;
static uint32_t cfgDirtyAt = 0;

static WebServer server(80);
#if CAPTIVE_PORTAL
static DNSServer dns;
#endif

#define LOCK() xSemaphoreTake(engMtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(engMtx)

static void applyHardware(const bc::Config& c) {
  hwApply(0, c.pins[0]);
  hwApply(1, c.pins[1]);
}

// ---- LED task: 250 Hz, on its own so the LEDs stay smooth while the web server is busy -------------------------------
static void ledTask(void*) {
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(4) > 0 ? pdMS_TO_TICKS(4) : 1;
  for (;;) {
    bc::Out o;
    LOCK();
    eng.render(esp_timer_get_time(), o);
    float master = eng.cfg.master / 100.f;
    if (otaBusy) {                                  // firmware update running: timer LED breathes cyan, set LED off
      float br = 0.3f + 0.25f * sinf((float)(esp_timer_get_time() % 1500000) / 1500000.f * bc::TAU_F);
      o.c1 = bc::Rgb{0.f, 170.f, 255.f}; o.b1 = br; o.b2 = 0.f;
    }
    hwWrite(0, o.c1, o.b1, master);
    hwWrite(1, o.c2, o.b2, master);
    UNLOCK();
    vTaskDelayUntil(&last, period);
  }
}

// ---- settings storage ---------------------------------------------------------------------------------------------
static void cfgSaveNow() {
  prefs.putBytes("cfg", cfgRaw.c_str(), cfgRaw.length());
  cfgDirty = false;
}

static void cfgLoad() {
  size_t n = prefs.getBytesLength("cfg");
  bc::Config c;
  c.setDefaults();
  if (n > 0 && n <= CFG_MAX_BYTES) {
    char* buf = (char*)malloc(n + 1);
    if (buf) {
      prefs.getBytes("cfg", buf, n);
      buf[n] = 0;
      bool pinsOk = true;
      if (bc::cfgFromJson(buf, n, c, &pinsOk)) cfgRaw = String(buf);
      else c.setDefaults();
      free(buf);
    }
  }
  eng.configure(c);
  applyHardware(eng.cfg);
}

// ---- web API ------------------------------------------------------------------------------------------------------
static void sendJson(int code, const char* body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendState() {
  int64_t now = esp_timer_get_time();
  char buf[200];
  LOCK();
  double t = eng.position(now);
  if (t > eng.plan.total) t = eng.plan.total;
  snprintf(buf, sizeof buf, "{\"app\":\"beacon\",\"ver\":\"%s\",\"s\":\"%s\",\"t\":%.3f,\"total\":%.1f,\"rev\":%lu,\"ota\":%d,\"up\":%lu}",
           FW_VERSION, bc::stateName(eng.state), t, (double)eng.plan.total, (unsigned long)cfgRev, otaBusy ? 1 : 0, (unsigned long)(millis() / 1000));
  UNLOCK();
  sendJson(200, buf);
}

static void handleCmd() {
  String c = server.arg("c");
  bool haveT = server.hasArg("t");
  double t = haveT ? server.arg("t").toDouble() : 0.0;
  int64_t now = esp_timer_get_time();
  LOCK();
  if (c == "start") eng.start(now, haveT, t);
  else if (c == "pause") eng.pause(now);
  else if (c == "stop") eng.stop(now);
  else if (c == "reset") eng.reset();
  else if (c == "seek" && haveT) eng.seek(now, t);
  UNLOCK();
  sendState();
}

static void handleConfigGet() {
  sendJson(200, cfgRaw.length() ? cfgRaw.c_str() : "{}");
}

static void handleConfigPost() {
  if (!server.hasArg("plain")) { sendJson(400, "{\"ok\":false,\"err\":\"no body\"}"); return; }
  const String& body = server.arg("plain");
  if (body.length() > CFG_MAX_BYTES) { sendJson(413, "{\"ok\":false,\"err\":\"too big\"}"); return; }
  bc::Config c;
  bool pinsOk = true;
  if (!bc::cfgFromJson(body.c_str(), body.length(), c, &pinsOk)) { sendJson(400, "{\"ok\":false,\"err\":\"bad json\"}"); return; }
  LOCK();
  eng.configure(c);
  applyHardware(eng.cfg);
  UNLOCK();
  if (body != cfgRaw) {
    cfgRaw = body;
    cfgDirty = true;                  // written to flash after a quiet moment, not on every slider tick
    cfgDirtyAt = millis();
  }
  cfgRev++;
  char buf[64];
  snprintf(buf, sizeof buf, "{\"ok\":true,\"rev\":%lu,\"pins\":%s}", (unsigned long)cfgRev, pinsOk ? "true" : "false");
  sendJson(200, buf);
}

static void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", (const char*)WEBUI_GZ, WEBUI_GZ_LEN);
}

// Home-screen icon (iOS "Add to Home Screen" reads apple-touch-icon) and the web app manifest.
static void sendIcon(const uint8_t* data, size_t len) {
  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send_P(200, "image/png", (const char*)data, len);
}
static void handleIcon180() { sendIcon(ICON_180, ICON_180_LEN); }
static void handleIcon192() { sendIcon(ICON_192, ICON_192_LEN); }
static void handleManifest() {
  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send(200, "application/manifest+json",
              "{\"name\":\"Beacon Timer\",\"short_name\":\"Beacon\",\"start_url\":\"/\",\"scope\":\"/\",\"display\":\"standalone\","
              "\"orientation\":\"any\",\"background_color\":\"#0b0e10\",\"theme_color\":\"#0b0e10\","
              "\"icons\":[{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any\"}]}");
}

// Saved timers. The page keeps the list (name + settings of each); the board only stores it, so every phone sees the same list.
static void handlePresetsGet() {
  server.sendHeader("Cache-Control", "no-store");
  if (fsOk && LittleFS.exists("/presets.json")) {
    File f = LittleFS.open("/presets.json", "r");
    if (f) { server.streamFile(f, "application/json"); f.close(); return; }
  }
  server.send(200, "application/json", "[]");
}
static void handlePresetsPost() {
  if (!fsOk) { sendJson(503, "{\"ok\":false,\"err\":\"no storage\"}"); return; }
  if (!server.hasArg("plain")) { sendJson(400, "{\"ok\":false,\"err\":\"no body\"}"); return; }
  const String& body = server.arg("plain");
  size_t n = body.length();
  if (n > PRESETS_MAX_BYTES) { sendJson(413, "{\"ok\":false,\"err\":\"too big\"}"); return; }
  if (!bc::presetsJsonOk(body.c_str(), n)) { sendJson(400, "{\"ok\":false,\"err\":\"bad json\"}"); return; }
  File f = LittleFS.open("/presets.tmp", "w");                       // write the new list beside the old one, then swap
  if (!f) { sendJson(500, "{\"ok\":false}"); return; }
  size_t w = f.write((const uint8_t*)body.c_str(), n);
  f.close();
  if (w != n) { LittleFS.remove("/presets.tmp"); sendJson(500, "{\"ok\":false}"); return; }
  LittleFS.remove("/presets.json");
  LittleFS.rename("/presets.tmp", "/presets.json");
  sendJson(200, "{\"ok\":true}");
}

static void handleReboot() {
  sendJson(200, "{\"ok\":true}");
  rebootAtMs = millis() + 600;
}

// Firmware upload from the web page (multipart POST to /update).
static void handleUpdateDone() {
  bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  sendJson(ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"ok\":false}");
  if (ok) rebootAtMs = millis() + 800;
  otaBusy = false;
}

static void handleUpdateData() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaBusy = true;
    Serial.printf("HTTP update: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("HTTP update done, %u bytes\n", (unsigned)up.totalSize);
    else Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaBusy = false;
  }
}

static void handleNotFound() {
  // Captive portal: any other address (connectivity checks, typed names) goes to the page.
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "");
}

// ---- setup / loop -------------------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);                 // USB CDC (CDC On Boot); does not wait for a terminal
  engMtx = xSemaphoreCreateMutex();

  prefs.begin("beacon", false);
  hw[0].live = hw[1].live = false;
  fsOk = LittleFS.begin(true);          // formats the storage on the very first boot
  cfgLoad();                            // also sets up the LED pins: common pin held HIGH (anode) so the LEDs start dark
  xTaskCreatePinnedToCore(ledTask, "led", 6144, nullptr, 5, nullptr, 1);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_CLIENTS);       // no password
  WiFi.setSleep(false);
#if CAPTIVE_PORTAL
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
#endif

  server.on("/", HTTP_GET, handleRoot);
  server.on("/apple-touch-icon.png", HTTP_GET, handleIcon180);
  server.on("/apple-touch-icon-precomposed.png", HTTP_GET, handleIcon180);
  server.on("/icon-192.png", HTTP_GET, handleIcon192);
  server.on("/favicon.ico", HTTP_GET, handleIcon192);
  server.on("/manifest.webmanifest", HTTP_GET, handleManifest);
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/api/presets", HTTP_GET, handlePresetsGet);
  server.on("/api/presets", HTTP_POST, handlePresetsPost);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateData);
  server.onNotFound(handleNotFound);
  server.begin();

  ArduinoOTA.setHostname(HOST_NAME);
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.onStart([]() { otaBusy = true; Serial.println("OTA start"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA end"); });
  ArduinoOTA.onError([](ota_error_t e) { otaBusy = false; Serial.printf("OTA error %u\n", (unsigned)e); });
  ArduinoOTA.begin();

  Serial.printf("Beacon Timer %s\n", FW_VERSION);
  Serial.printf("AP \"%s\" (open)  http://%s  OTA port %d\n", AP_SSID, WiFi.softAPIP().toString().c_str(), OTA_PORT);
}

void loop() {
#if CAPTIVE_PORTAL
  dns.processNextRequest();
#endif
  server.handleClient();
  ArduinoOTA.handle();

  if (cfgDirty && millis() - cfgDirtyAt > 2000) cfgSaveNow();
  if (rebootAtMs && (int32_t)(millis() - rebootAtMs) >= 0) {
    if (cfgDirty) cfgSaveNow();
    delay(100);
    ESP.restart();
  }
  delay(2);
}
