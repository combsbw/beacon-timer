/*
 * Beacon Timer - interval timer with two RGB LEDs, for the ESP32-S3 Super Mini
 *
 *   Board   : esp32:esp32:esp32s3  (arduino-esp32 core 2.0.17), USB CDC On Boot = enabled
 *   Wi-Fi   : open soft AP "Beacon-timer"  ->  http://192.168.4.1  (captive portal opens the page by itself)
 *             or, after Device > Network, the home network saved for online update -> http://beacon-timer.local
 *   Sleep   : home-network mode only. LEDs off and Wi-Fi modem sleep after 5 min idle, or when the page asks.
 *             A wake request (or starting a session) brings the radio and LEDs back. The AP never sleeps.
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
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "engine.h"
#include "json.h"
#include "config.h"
#include "webui.h"
#include "icons.h"
#include "ghupdate.h"

#define FW_VERSION "0.4.0"
#define IDLE_SLEEP_MS (5UL * 60UL * 1000UL)   // home-network mode: sleep this long after the session stops
#define STA_JOIN_MS 15000
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
static volatile bool powerSave = false;     // home-network modem sleep: LEDs off, radio wakes for a packet
static volatile uint8_t joinPhase = 0;      // 1 while the boot join of the home network is in progress
static bool radioSta = false;              // this boot is associated to the home network
static bool staFailed = false;             // wanted the home network, join failed, so this boot is the access point
static bool dnsOn = false;
static uint32_t idleSinceMs = 0;
static TaskHandle_t ledTaskHandle = nullptr;
// Online update from GitHub: phase 1 joining Wi-Fi, 2 clock, 3 finding the release, 4 downloading, 5 done, 6 failed (0 = idle)
static volatile uint8_t updPhase = 0, updPct = 0, updErr = 0;
static volatile uint32_t updStartAt = 0;

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

// ---- LED patterns while an online update runs: proof of life, which step, how far, and why it failed -----------------
//   Timer LED: slow breathing (amber while connecting, cyan while downloading), solid green when done, fast red blink on failure.
//   Set LED:   steps 1-3 flash 1, 2 or 3 times then pause; download = a red-to-green bar color with a heartbeat dip each second;
//              failure = flashes the error number (1 Wi-Fi, 2 clock, 3 GitHub, 4 download, 5 flash write), pause, repeat.
static void updLeds(bc::Out& o) {
  uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  uint8_t ph = updPhase;
  float breath = 0.3f + 0.25f * sinf((float)(ms % 1500) / 1500.f * bc::TAU_F);
  const bc::Rgb white = {255.f, 255.f, 255.f};
  if (ph >= 1 && ph <= 3) {
    o.c1 = bc::Rgb{255.f, 170.f, 0.f}; o.b1 = breath;
    uint32_t t = ms % 2400, slot = t / 350;
    o.c2 = white; o.b2 = (slot < ph && (t % 350) < 150) ? 0.9f : 0.f;
  } else if (ph == 4) {
    float f = updPct / 100.f;
    bc::Rgb a = {255.f, 40.f, 0.f}, m = {255.f, 200.f, 0.f}, g = {30.f, 230.f, 90.f};
    o.c1 = bc::Rgb{0.f, 170.f, 255.f}; o.b1 = breath;
    o.c2 = f < 0.5f ? bc::mixRgb(a, m, f * 2.f) : bc::mixRgb(m, g, (f - 0.5f) * 2.f);
    o.b2 = (ms % 1000) < 120 ? 0.25f : 0.9f;
  } else if (ph == 5) {
    o.c1 = bc::Rgb{34.f, 224.f, 112.f}; o.b1 = 1.f; o.c2 = o.c1; o.b2 = 1.f;
  } else {
    o.c1 = bc::Rgb{255.f, 31.f, 45.f}; o.b1 = ((ms / 125) & 1) ? 1.f : 0.05f;
    uint32_t n = updErr ? updErr : 1, cyc = n * 350 + 1200, t = ms % cyc;
    o.c2 = white; o.b2 = (t < n * 350 && (t % 350) < 150) ? 0.9f : 0.f;
  }
}

// ---- LED task: 250 Hz, on its own so the LEDs stay smooth while the web server is busy -------------------------------
static void leaveSleep() {
  if (!powerSave) return;
  powerSave = false;
  esp_wifi_set_ps(WIFI_PS_NONE);            // polls and the LED frame rate stay responsive while someone is using it
  idleSinceMs = millis();
  if (ledTaskHandle) xTaskNotifyGive(ledTaskHandle);
}

static void enterSleep() {
  if (!radioSta || powerSave || otaBusy || updPhase) return;
  powerSave = true;
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);       // radio stays associated and wakes when a packet arrives
}

static void noteActivity() {
  idleSinceMs = millis();
  if (powerSave) leaveSleep();
}

// Asleep only while the home network is up and no session is running or paused.
static void refreshIdle() {
  if (!radioSta) return;
  if (otaBusy || updPhase || rebootAtMs || updStartAt) {
    if (powerSave) leaveSleep();
    return;
  }
  bool busy;
  LOCK();
  busy = eng.state == bc::ST_RUN || eng.state == bc::ST_PAUSE;
  UNLOCK();
  if (busy) {
    if (powerSave) leaveSleep();
    idleSinceMs = millis();
    return;
  }
  if (!powerSave && (uint32_t)(millis() - idleSinceMs) >= IDLE_SLEEP_MS) enterSleep();
}

static void ledTask(void*) {
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(4) > 0 ? pdMS_TO_TICKS(4) : 1;
  const bc::Rgb dark = {0.f, 0.f, 0.f};
  for (;;) {
    if (powerSave) {
      LOCK();
      hwWrite(0, dark, 0.f, 0.f);
      hwWrite(1, dark, 0.f, 0.f);
      UNLOCK();
      while (powerSave) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // a pending notify from a previous wake is consumed and we wait again
      last = xTaskGetTickCount();
      continue;
    }
    bc::Out o;
    LOCK();
    eng.render(esp_timer_get_time(), o);
    float master = eng.cfg.master / 100.f;
    if (joinPhase == 1) {
      float br = 0.3f + 0.25f * sinf((float)(esp_timer_get_time() % 1500000) / 1500000.f * bc::TAU_F);
      o.c1 = bc::Rgb{255.f, 170.f, 0.f}; o.b1 = br; o.b2 = 0.f;
    } else if (updPhase) {
      updLeds(o);
    } else if (otaBusy) {                           // firmware update running: timer LED breathes cyan, set LED off
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
  char buf[240];
  LOCK();
  double t = eng.position(now);
  if (t > eng.plan.total) t = eng.plan.total;
  snprintf(buf, sizeof buf,
           "{\"app\":\"beacon\",\"ver\":\"%s\",\"s\":\"%s\",\"t\":%.3f,\"total\":%.1f,\"rev\":%lu,\"ota\":%d,\"up\":%lu,\"sleep\":%d,\"net\":\"%s\",\"fail\":%d}",
           FW_VERSION, bc::stateName(eng.state), t, (double)eng.plan.total, (unsigned long)cfgRev, otaBusy ? 1 : 0,
           (unsigned long)(millis() / 1000), powerSave ? 1 : 0, radioSta ? "sta" : "ap", staFailed ? 1 : 0);
  UNLOCK();
  sendJson(200, buf);
}

static void handleWake() {
  noteActivity();
  sendState();
}

static void handleSleep() {
  if (!radioSta) { sendJson(409, "{\"ok\":false,\"err\":\"ap\"}"); return; }
  LOCK();
  bool running = eng.state == bc::ST_RUN || eng.state == bc::ST_PAUSE;
  UNLOCK();
  if (running || otaBusy || updPhase) { sendJson(409, "{\"ok\":false,\"err\":\"busy\"}"); return; }
  enterSleep();
  sendState();
}

static void handleCmd() {
  noteActivity();
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
  noteActivity();
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
  noteActivity();
  sendJson(200, "{\"ok\":true}");
}

// ---- online update from GitHub ------------------------------------------------------------------------------------
// The page stores a Wi-Fi network (and optionally a repo / token for a private repo). "Update" leaves the soft AP, joins that
// network, downloads the newest .bin from the repo's "latest" release over verified HTTPS, installs it and restarts, which
// brings the soft AP back. Result and failure reason are kept in flash for the page to show afterwards.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

static String jsonEsc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

// Never sends the saved password or token back, only whether they exist.
static void handleUpdInfo() {
  String ssid = prefs.getString("wSsid", ""), repo = prefs.getString("wRepo", GH_REPO_DEFAULT);
  bool hasPass = prefs.getString("wPass", "").length() > 0, hasTok = prefs.getString("wTok", "").length() > 0;
  String r = String("{\"ssid\":\"") + jsonEsc(ssid) + "\",\"hasPass\":" + (hasPass ? "true" : "false") + ",\"repo\":\"" + jsonEsc(repo) +
             "\",\"hasToken\":" + (hasTok ? "true" : "false") + ",\"last\":\"" + jsonEsc(prefs.getString("wRes", "")) +
             "\",\"from\":\"" + jsonEsc(prefs.getString("wFrom", "")) + "\",\"ver\":\"" FW_VERSION "\"}";
  sendJson(200, r.c_str());
}

// Form fields (any subset): ssid, pass, repo, token. A field that is not sent stays as it was; token "" clears it.
static void handleUpdSave() {
  if (server.hasArg("ssid")) { String v = server.arg("ssid"); if (v.length() > 32) { sendJson(400, "{\"ok\":false,\"err\":\"ssid\"}"); return; } prefs.putString("wSsid", v); }
  if (server.hasArg("pass")) { String v = server.arg("pass"); if (v.length() > 63) { sendJson(400, "{\"ok\":false,\"err\":\"pass\"}"); return; } prefs.putString("wPass", v); }
  if (server.hasArg("repo")) {
    String v = server.arg("repo");
    if (v.length() == 0) v = GH_REPO_DEFAULT;
    if (!bc::ghRepoOk(v.c_str())) { sendJson(400, "{\"ok\":false,\"err\":\"repo\"}"); return; }
    prefs.putString("wRepo", v);
  }
  if (server.hasArg("token")) {
    String v = server.arg("token");
    if (!bc::ghTokenOk(v.c_str())) { sendJson(400, "{\"ok\":false,\"err\":\"token\"}"); return; }
    prefs.putString("wTok", v);
  }
  sendJson(200, "{\"ok\":true}");
}

static void handleUpdStart() {
  if (prefs.getString("wSsid", "").length() == 0) { sendJson(400, "{\"ok\":false,\"err\":\"ssid\"}"); return; }
  LOCK();
  bool running = eng.state == bc::ST_RUN || eng.state == bc::ST_PAUSE;
  UNLOCK();
  if (running || otaBusy || updPhase) { sendJson(409, "{\"ok\":false,\"err\":\"busy\"}"); return; }
  sendJson(200, "{\"ok\":true}");
  updStartAt = millis() + 900;            // let this reply reach the phone before the access point goes away
  if (!updStartAt) updStartAt = 1;
}

static bool updFail(uint8_t code, const char* why) {
  updErr = code;
  Serial.printf("online update failed (%u): %s\n", (unsigned)code, why);
  return false;
}

// One request, following redirects by hand so the token only goes to api.github.com and never to the file host.
static bool ghFetch(WiFiClientSecure& cl, HTTPClient& http, String url, const String& tok, const char* accept) {
  for (int hop = 0; hop < 6; hop++) {
    char host[80];
    bc::ghUrlHost(url.c_str(), host, sizeof host);
    if (!host[0]) return false;
    http.end();
    cl.stop();
    if (!http.begin(cl, url)) return false;
    http.setUserAgent("beacon-timer");
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    http.addHeader("Accept", accept);
    if (tok.length() && bc::ghIsApiHost(host)) http.addHeader("Authorization", String("Bearer ") + tok);
    const char* keys[] = {"Location"};
    http.collectHeaders(keys, 1);
    int code = http.GET();
    if (code == 200) return true;
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      url = http.header("Location");
      if (url.length() == 0) return false;
      continue;
    }
    Serial.printf("HTTP %d from %s\n", code, host);
    return false;
  }
  return false;
}

static void runWebUpdate() {
  if (cfgDirty) cfgSaveNow();
  String ssid = prefs.getString("wSsid", ""), pass = prefs.getString("wPass", "");
  String repo = prefs.getString("wRepo", GH_REPO_DEFAULT), tok = prefs.getString("wTok", "");
  prefs.putString("wFrom", FW_VERSION);
  prefs.putString("wRes", "");
  otaBusy = true; updErr = 0; updPct = 0; updPhase = 1;
  if (powerSave) leaveSleep();          // the LED task is parked while asleep, and the update pattern has to be visible
  Serial.println("online update: joining Wi-Fi");

  server.stop();
#if CAPTIVE_PORTAL
  if (dnsOn) { dns.stop(); dnsOn = false; }
#endif
  ArduinoOTA.end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), pass.c_str());

  bool ok = true;
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) delay(100);
  if (WiFi.status() != WL_CONNECTED) ok = updFail(bc::UE_WIFI, "could not join the Wi-Fi network");

  if (ok) {                                // certificate dates are checked, so the clock must be right
    updPhase = 2;
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
    t0 = millis();
    while (time(nullptr) < 1700000000 && millis() - t0 < 15000) delay(100);
    if (time(nullptr) < 1700000000) ok = updFail(bc::UE_TIME, "no internet time");
  }

  WiFiClientSecure cl;
  HTTPClient http;
  bool begun = false;
  if (ok) {
    updPhase = 3;
    cl.setCACertBundle(rootca_crt_bundle_start);
    String url = String("https://github.com/") + repo + "/releases/download/" GH_TAG "/" GH_ASSET;
    const char* accept = "application/octet-stream";
    if (tok.length()) {                    // private repo: ask the API for the asset, then fetch it with the token
      url = String("https://api.github.com/repos/") + repo + "/releases/tags/" GH_TAG;
      if (!ghFetch(cl, http, url, tok, "application/vnd.github+json")) ok = updFail(bc::UE_LOOKUP, "release lookup");
      else {
        String js = http.getString();
        http.end();
        char au[200];
        if (!bc::ghFindAssetUrl(js.c_str(), js.length(), GH_ASSET, au, sizeof au)) ok = updFail(bc::UE_LOOKUP, "asset not in release");
        else url = au;
      }
    }
    if (ok && !ghFetch(cl, http, url, tok, accept)) ok = updFail(bc::UE_LOOKUP, "download request");
  }

  if (ok) {
    int len = http.getSize();
    if (len < 200000 || len > 0x140000) ok = updFail(bc::UE_DOWNLOAD, "unexpected file size");
    else if (!Update.begin((size_t)len, U_FLASH)) ok = updFail(bc::UE_FLASH, "no room for the update");
    else {
      begun = true;
      updPhase = 4;
      WiFiClient* stream = http.getStreamPtr();
      uint8_t buf[1024];
      size_t done = 0;
      uint32_t lastData = millis();
      while (done < (size_t)len) {
        size_t avail = stream->available();
        if (avail) {
          size_t want = avail < sizeof buf ? avail : sizeof buf;
          if (want > (size_t)len - done) want = (size_t)len - done;
          int n = stream->readBytes(buf, want);
          if (n <= 0) continue;
          if (Update.write(buf, (size_t)n) != (size_t)n) { ok = updFail(bc::UE_FLASH, "flash write"); break; }
          done += (size_t)n;
          updPct = (uint8_t)((uint64_t)done * 100 / (size_t)len);
          lastData = millis();
        } else {
          if (!stream->connected()) break;
          if (millis() - lastData > 20000) { ok = updFail(bc::UE_DOWNLOAD, "download stalled"); break; }
          delay(2);
        }
      }
      if (ok && done != (size_t)len) ok = updFail(bc::UE_DOWNLOAD, "download ended early");
      if (ok && !Update.end(true)) ok = updFail(bc::UE_FLASH, "image check failed");
    }
  }
  http.end();
  if (!ok && begun) Update.abort();

  if (ok) {
    prefs.putString("wRes", "ok");
    updPhase = 5;
    Serial.println("online update installed, restarting");
    delay(2500);                           // solid green so you can see it worked
  } else {
    prefs.putString("wRes", String("e") + String((unsigned)updErr));
    updPhase = 6;
    delay(12000);                          // error flashes
  }
  ESP.restart();                           // either way the soft AP comes back with the next boot
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

static void handleNetInfo() {
  String ssid = prefs.getString("wSsid", "");
  String wanted = prefs.getString("wMode", "ap");
  if (wanted != "sta") wanted = "ap";
  bool hasPass = prefs.getString("wPass", "").length() > 0;
  String ip = radioSta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String r = String("{\"wanted\":\"") + wanted + "\",\"radio\":\"" + (radioSta ? "sta" : "ap") +
             "\",\"fail\":" + (staFailed ? "true" : "false") + ",\"ssid\":\"" + jsonEsc(ssid) +
             "\",\"hasPass\":" + (hasPass ? "true" : "false") + ",\"ip\":\"" + ip +
             "\",\"host\":\"" HOST_NAME "\",\"sleep\":" + (powerSave ? "true" : "false") + "}";
  sendJson(200, r.c_str());
}

// mode=sta saves the shared Wi-Fi name and password (the same ones online update uses) and reboots into station mode.
// mode=ap reboots into the always-on access point. A field that is not sent is left as it was.
static void handleNetPost() {
  if (!server.hasArg("mode")) { sendJson(400, "{\"ok\":false,\"err\":\"mode\"}"); return; }
  String mode = server.arg("mode");
  if (mode != "sta" && mode != "ap") { sendJson(400, "{\"ok\":false,\"err\":\"mode\"}"); return; }
  LOCK();
  bool running = eng.state == bc::ST_RUN || eng.state == bc::ST_PAUSE;
  UNLOCK();
  if (running || otaBusy || updPhase) { sendJson(409, "{\"ok\":false,\"err\":\"busy\"}"); return; }
  if (server.hasArg("ssid")) {
    String v = server.arg("ssid");
    if (v.length() > 32) { sendJson(400, "{\"ok\":false,\"err\":\"ssid\"}"); return; }
    prefs.putString("wSsid", v);
  }
  if (server.hasArg("pass")) {
    String v = server.arg("pass");
    if (v.length() > 63) { sendJson(400, "{\"ok\":false,\"err\":\"pass\"}"); return; }
    prefs.putString("wPass", v);
  }
  if (mode == "sta" && prefs.getString("wSsid", "").length() == 0) { sendJson(400, "{\"ok\":false,\"err\":\"ssid\"}"); return; }
  prefs.putString("wMode", mode);
  sendJson(200, "{\"ok\":true}");
  rebootAtMs = millis() + 600;
  if (!rebootAtMs) rebootAtMs = 1;
}

static void handleNotFound() {
  if (radioSta) { server.send(404, "text/plain", "not found"); return; }
  // Captive portal: any other address (connectivity checks, typed names) goes to the page.
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "");
}

static bool startStation() {
  String ssid = prefs.getString("wSsid", "");
  String pass = prefs.getString("wPass", "");
  if (!ssid.length()) return false;
  joinPhase = 1;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOST_NAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - t0) < STA_JOIN_MS) delay(100);
  joinPhase = 0;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("home Wi-Fi join failed for \"%s\"\n", ssid.c_str());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    return false;
  }
  esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("home Wi-Fi \"%s\"  http://%s  http://%s.local\n", ssid.c_str(), WiFi.localIP().toString().c_str(), HOST_NAME);
  return true;
}

static void startAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAPsetHostname(HOST_NAME);
  WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_CLIENTS);       // no password
  esp_wifi_set_ps(WIFI_PS_NONE);
#if CAPTIVE_PORTAL
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
  dnsOn = true;
#endif
}

// ---- setup / loop -------------------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);                 // USB CDC (CDC On Boot); does not wait for a terminal
  engMtx = xSemaphoreCreateMutex();

  prefs.begin("beacon", false);
  hw[0].live = hw[1].live = false;
  fsOk = LittleFS.begin(true);          // formats the storage on the very first boot
  cfgLoad();                            // also sets up the LED pins: common pin held HIGH (anode) so the LEDs start dark
  xTaskCreatePinnedToCore(ledTask, "led", 6144, nullptr, 5, &ledTaskHandle, 1);

  WiFi.persistent(false);
  String wanted = prefs.getString("wMode", "ap");
  if (wanted == "sta") {
    radioSta = startStation();
    staFailed = !radioSta;
  }
  if (!radioSta) startAp();
  else if (MDNS.begin(HOST_NAME)) MDNS.addService("http", "tcp", 80);
  idleSinceMs = millis();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/apple-touch-icon.png", HTTP_GET, handleIcon180);
  server.on("/apple-touch-icon-precomposed.png", HTTP_GET, handleIcon180);
  server.on("/icon-192.png", HTTP_GET, handleIcon192);
  server.on("/favicon.ico", HTTP_GET, handleIcon192);
  server.on("/manifest.webmanifest", HTTP_GET, handleManifest);
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/api/net", HTTP_GET, handleNetInfo);
  server.on("/api/net", HTTP_POST, handleNetPost);
  server.on("/api/wake", HTTP_POST, handleWake);
  server.on("/api/sleep", HTTP_POST, handleSleep);
  server.on("/api/update", HTTP_GET, handleUpdInfo);
  server.on("/api/update/save", HTTP_POST, handleUpdSave);
  server.on("/api/update/start", HTTP_POST, handleUpdStart);
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
  ArduinoOTA.onStart([]() { otaBusy = true; if (powerSave) leaveSleep(); Serial.println("OTA start"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA end"); });
  ArduinoOTA.onError([](ota_error_t e) { otaBusy = false; Serial.printf("OTA error %u\n", (unsigned)e); });
  ArduinoOTA.begin();

  Serial.printf("Beacon Timer %s\n", FW_VERSION);
  if (radioSta) Serial.printf("STA http://%s  http://%s.local  OTA port %d\n", WiFi.localIP().toString().c_str(), HOST_NAME, OTA_PORT);
  else Serial.printf("AP \"%s\" (open)  http://%s  OTA port %d%s\n", AP_SSID, WiFi.softAPIP().toString().c_str(), OTA_PORT, staFailed ? "  (home Wi-Fi join failed)" : "");
}

void loop() {
#if CAPTIVE_PORTAL
  if (dnsOn) dns.processNextRequest();
#endif
  server.handleClient();
  ArduinoOTA.handle();
  refreshIdle();

  if (updStartAt && (int32_t)(millis() - updStartAt) >= 0) { updStartAt = 0; runWebUpdate(); }   // does not return: it restarts
  if (cfgDirty && millis() - cfgDirtyAt > 2000) cfgSaveNow();
  if (rebootAtMs && (int32_t)(millis() - rebootAtMs) >= 0) {
    if (cfgDirty) cfgSaveNow();
    delay(100);
    ESP.restart();
  }
  delay(powerSave ? 20 : 2);
}
