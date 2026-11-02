// config.h - reads the settings JSON that the web UI sends into a bc::Config, and checks it.
// Keys the engine doesn't need (names, theme, ...) are ignored; the sketch stores the original text and hands it back
// to the UI unchanged. Anything missing keeps its default, anything out of range is clamped.
#pragma once
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "json.h"

namespace bc {

// GPIOs the UI offers on the ESP32-S3: not 0 (boot strap), 19/20 (USB), 26-32 (flash/PSRAM).
inline bool pinAllowed(int p) { return (p >= 1 && p <= 18) || p == 21; }

struct CfgCtx {
  Config* c;
  int colorN[3];
};

inline bool cfgPre(const char* p, const char* pfx, const char** rest) {
  size_t n = strlen(pfx);
  if (strncmp(p, pfx, n) != 0) return false;
  *rest = p + n;
  return true;
}

inline void cfgPin(PinCfg& pin, const char* k, char t, double v) {
  int iv = (int)floor(clampd(v, -1.0, 127.0) + 0.5);
  if (t == 'b') { if (!strcmp(k, "anode")) pin.anode = v != 0; return; }
  if (!strcmp(k, "r")) pin.r = (int8_t)iv;
  else if (!strcmp(k, "g")) pin.g = (int8_t)iv;
  else if (!strcmp(k, "b")) pin.b = (int8_t)iv;
  else if (!strcmp(k, "com")) pin.com = (int8_t)iv;
}

inline void cfgProfileNum(Profile& p, const char* k, float v) {
  if (!strcmp(k, "rate0")) p.rate0 = v;
  else if (!strcmp(k, "rate1")) p.rate1 = v;
  else if (!strcmp(k, "curve")) p.curve = (int)floorf(v + 0.5f);
  else if (!strcmp(k, "duty0")) p.duty0 = v;
  else if (!strcmp(k, "duty1")) p.duty1 = v;
  else if (!strcmp(k, "soft0")) p.soft0 = v;
  else if (!strcmp(k, "soft1")) p.soft1 = v;
  else if (!strcmp(k, "floor")) p.glow = v;
  else if (!strcmp(k, "blend")) p.blend = v;
  else if (!strcmp(k, "solid")) p.solid = v;
  else if (!strcmp(k, "fade")) p.fade = v;
  else if (!strcmp(k, "breath")) p.breath = v;
  else if (!strcmp(k, "tail")) p.tail = v;
  else if (!strcmp(k, "f0")) p.f0 = v;
  else if (!strcmp(k, "f1")) p.f1 = v;
}

inline bool cfgHex(const char* s, Rgb& out) {
  if (s[0] != '#' || strlen(s) != 7) return false;
  uint32_t v = 0;
  for (int i = 1; i < 7; i++) {
    char c = s[i];
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    v = (v << 4) | (uint32_t)d;
  }
  out = Config::hex(v);
  return true;
}

inline void cfgLeaf(void* vctx, const char* p, char t, double v, const char* s) {
  CfgCtx& x = *(CfgCtx*)vctx;
  Config& c = *x.c;
  const char* r;
  if (t == 'n' || t == 'b') {
    float f = (float)v;
    if (!strcmp(p, "loadSec")) c.loadSec = f;
    else if (!strcmp(p, "transSec")) c.transSec = f;
    else if (!strcmp(p, "sets")) c.sets = (int)floor(v + 0.5);
    else if (!strcmp(p, "repeat")) c.repeat = (int)floor(v + 0.5);
    else if (!strcmp(p, "redHold")) c.redHold = f;
    else if (!strcmp(p, "skipFinal")) c.skipFinal = v != 0;
    else if (!strcmp(p, "lead")) c.lead = v != 0;
    else if (!strcmp(p, "leadSame")) c.leadSame = v != 0;
    else if (!strcmp(p, "leadSec")) c.leadSec = f;
    else if (!strcmp(p, "holdMs")) c.holdMs = f;
    else if (!strcmp(p, "maxHz")) c.maxHz = f;
    else if (!strcmp(p, "master")) c.master = f;
    else if (cfgPre(p, "pins.timer.", &r)) cfgPin(c.pins[0], r, t, v);
    else if (cfgPre(p, "pins.set.", &r)) cfgPin(c.pins[1], r, t, v);
    else if (t == 'n' && cfgPre(p, "profiles.load.", &r)) cfgProfileNum(c.load, r, f);
    else if (t == 'n' && cfgPre(p, "profiles.trans.", &r)) cfgProfileNum(c.trans, r, f);
    else if (t == 'n' && cfgPre(p, "profiles.lead.", &r)) cfgProfileNum(c.leadP, r, f);
  } else if (t == 's') {
    Profile* pr = nullptr;
    int k = 0;
    if (cfgPre(p, "profiles.load.colors.", &r)) { pr = &c.load; k = 0; }
    else if (cfgPre(p, "profiles.trans.colors.", &r)) { pr = &c.trans; k = 1; }
    else if (cfgPre(p, "profiles.lead.colors.", &r)) { pr = &c.leadP; k = 2; }
    if (pr) {
      int idx = atoi(r);
      Rgb col;
      if (idx >= 0 && idx < MAX_STOPS && cfgHex(s, col)) {
        pr->colors[idx] = col;
        if (idx + 1 > x.colorN[k]) x.colorN[k] = idx + 1;
      }
    }
  }
}

inline void cfgSanitizeProfile(Profile& p) {
  p.rate0 = clampf(p.rate0, 0.5f, 300.f); p.rate1 = clampf(p.rate1, 0.5f, 300.f);
  if (p.curve < 1) p.curve = 1;
  if (p.curve > 3) p.curve = 3;
  p.duty0 = clampf(p.duty0, 5.f, 95.f); p.duty1 = clampf(p.duty1, 5.f, 95.f);
  p.soft0 = clampf(p.soft0, 0.f, 100.f); p.soft1 = clampf(p.soft1, 0.f, 100.f);
  p.solid = clampf(p.solid, 0.f, 10.f); p.fade = clampf(p.fade, 0.f, 5.f);
  p.glow = clampf(p.glow, 0.f, 100.f); p.blend = clampf(p.blend, 0.f, 100.f);
  p.breath = clampf(p.breath, 0.5f, 20.f); p.tail = clampf(p.tail, 0.5f, 60.f);
  p.f0 = clampf(p.f0, 0.1f, 12.f); p.f1 = clampf(p.f1, 0.1f, 12.f);
  if (p.nColors < 2) p.nColors = 2;
  if (p.nColors > MAX_STOPS) p.nColors = MAX_STOPS;
}

// Parses `json` over the defaults. Returns false only if the text is not valid JSON.
// *pinsOk is set false when the pin assignment was unusable (bad pin or a pin used twice); the default pins are then
// kept, everything else is still applied.
inline bool cfgFromJson(const char* json, size_t n, Config& out, bool* pinsOk = nullptr) {
  out.setDefaults();
  CfgCtx x;
  x.c = &out;
  x.colorN[0] = x.colorN[1] = x.colorN[2] = 0;
  if (!jsonWalk(json, n, cfgLeaf, &x)) return false;

  Profile* ps[3] = {&out.load, &out.trans, &out.leadP};
  for (int k = 0; k < 3; k++) {
    if (x.colorN[k] >= 2) ps[k]->nColors = x.colorN[k];
    cfgSanitizeProfile(*ps[k]);
  }
  out.loadSec = clampf(out.loadSec, 3.f, 600.f);
  out.transSec = clampf(out.transSec, 0.f, 600.f);
  if (out.sets < 1) out.sets = 1;
  if (out.sets > MAX_SETS) out.sets = MAX_SETS;
  if (out.repeat < 1) out.repeat = 1;
  if (out.repeat > MAX_SETS) out.repeat = MAX_SETS;
  out.redHold = clampf(out.redHold, 0.f, 10.f);
  out.leadSec = clampf(out.leadSec, 1.f, 600.f);
  out.holdMs = clampf(out.holdMs, 0.f, 1000.f);
  out.maxHz = clampf(out.maxHz, 1.f, 8.f);
  out.master = clampf(out.master, 10.f, 100.f);

  bool ok = true;
  for (int k = 0; k < 2; k++) {
    const PinCfg& p = out.pins[k];
    if (!pinAllowed(p.r) || !pinAllowed(p.g) || !pinAllowed(p.b) || !(p.com == -1 || pinAllowed(p.com))) ok = false;
  }
  if (ok) {
    int seen[8], ns = 0;
    for (int k = 0; k < 2 && ok; k++) {
      int pins[4] = {out.pins[k].r, out.pins[k].g, out.pins[k].b, out.pins[k].com};
      for (int j = 0; j < 4 && ok; j++) {
        if (pins[j] < 0) continue;
        for (int m = 0; m < ns; m++) if (seen[m] == pins[j]) ok = false;
        if (ok) seen[ns++] = pins[j];
      }
    }
  }
  if (!ok) {
    Config d;
    d.setDefaults();
    for (int k = 0; k < 2; k++) {
      bool anode = out.pins[k].anode;
      out.pins[k] = d.pins[k];
      out.pins[k].anode = anode;
    }
  }
  if (pinsOk) *pinsOk = ok;
  return true;
}

}  // namespace bc
