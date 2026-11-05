// engine.h - Beacon Timer engine. Plain C++ (no Arduino headers) so it also builds and tests on a PC.
//
// This is a line-for-line port of the engine in the web UI: the session plan (lead-in, load, red hold,
// transition), the accelerating pulse (rate, duty cycle and edge softness all run off one curve), the
// transition breathing + ramp, the set colors, and the timer state machine.
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>

namespace bc {

enum { MAX_SETS = 20, MAX_CYC = MAX_SETS * 3, MAX_STOPS = 5 };
static const double TAU_D = 6.283185307179586476925286766559;
static const float TAU_F = 6.28318530717958647692f;
static const float GAMMA_F = 2.2f;

struct Rgb { float r, g, b; };
static const Rgb HOLD_COL = {255.f, 31.f, 45.f};

inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
inline double clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothf(float a, float b, float x) {
  if (a == b) return x < a ? 0.f : 1.f;
  float t = clampf((x - a) / (b - a), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

// Mix two colors in (approximately) linear light, so green to red passes through yellow.
inline Rgb mixRgb(const Rgb& a, const Rgb& b, float t) {
  Rgb o;
  o.r = powf(lerpf(powf(a.r / 255.f, GAMMA_F), powf(b.r / 255.f, GAMMA_F), t), 1.f / GAMMA_F) * 255.f;
  o.g = powf(lerpf(powf(a.g / 255.f, GAMMA_F), powf(b.g / 255.f, GAMMA_F), t), 1.f / GAMMA_F) * 255.f;
  o.b = powf(lerpf(powf(a.b / 255.f, GAMMA_F), powf(b.b / 255.f, GAMMA_F), t), 1.f / GAMMA_F) * 255.f;
  return o;
}

inline float hslChan(float n, float h, float s, float l) {
  float k = fmodf(n + h * 12.f, 12.f);
  float m = fminf(fminf(k - 3.f, 9.f - k), 1.f);
  m = fmaxf(-1.f, m);
  return (l - s * fminf(l, 1.f - l) * m) * 255.f;
}
inline Rgb hslToRgb(float h, float s, float l) { return Rgb{hslChan(0.f, h, s, l), hslChan(8.f, h, s, l), hslChan(4.f, h, s, l)}; }

// Color across a window. K stops share the window equally; blend 0 = hard steps, 1 = smooth glide.
inline Rgb gradientAt(const Rgb* st, int K, float p, float blend) {
  if (K <= 1) return st[0];
  p = clampf(p, 0.f, 0.99999f);
  float x = p * K, w = clampf(blend, 0.f, 1.f);
  int j = (int)floorf(x + 0.5f);
  if (w > 0.f && j >= 1 && j <= K - 1) {
    float d = x - (float)j;
    if (fabsf(d) < w / 2.f) return mixRgb(st[j - 1], st[j], smoothf(-w / 2.f, w / 2.f, d));
  }
  int idx = (int)floorf(x);
  if (idx > K - 1) idx = K - 1;
  return st[idx];
}

// One pulse across a cycle phase x (0..1): lit for `duty` of the cycle, edges ramped by `soft`.
inline float pulseShape(float x, float duty, float soft) {
  if (x >= duty) return 0.f;
  float u = x / duty, e = clampf(soft * 0.5f, 0.015f, 0.5f);
  return smoothf(0.f, e, u) * (1.f - smoothf(1.f - e, 1.f, u));
}

// Accelerating rate f(p) = f0 + (f1 - f0) * p^n. q = p^n is the same curve as a 0..1 share (duty and softness follow it);
// cyc is the closed-form integral of the rate, i.e. the pulse phase in cycles. No accumulator, so it can be scrubbed.
struct Accel { float q; double cyc; };
inline Accel accelOf(double local, double len, double f0, double f1, int n) {
  double p = clampd(local / len, 0.0, 1.0), pn = pow(p, (double)n);
  Accel a;
  a.q = (float)pn;
  a.cyc = f0 * local + (f1 - f0) * len / (double)(n + 1) * pn * p;
  return a;
}

struct Profile {
  float rate0, rate1;          // pulses per window at the start and end (load)
  int curve;                   // 1 linear, 2 squared, 3 cubic
  float duty0, duty1, soft0, soft1;   // percent
  float finalSec;              // load: the last finalSec seconds run the accelerating pulse; a longer load pulses slowly through the colors before that
  float longCycle, longDim;    // load, long part: slow pulse cycle (s) and its dim level (percent)
  float markEvery;             // load, long part: seconds between time-mark double flashes (0 = off)
  float solid, fade;           // load: seconds of solid first color at the start of the window, then the fade into the pulses
  float glow;                  // percent, the dim level between pulses ("floor" in the UI)
  float blend;                 // percent, color crossfade
  float breath, tail, f0, f1;  // transition: breath cycle (s), ramp length (s), ramp Hz from / to
  int nColors;
  Rgb colors[MAX_STOPS];
};

struct PinCfg { int8_t r, g, b, com; bool anode; };

struct Config {
  float loadSec, transSec;
  bool trackRepeats;           // LED 2 follows the repeat (not the set)
  int sets, repeat;            // repeat = how many times the whole run of sets plays (1 = no repeat)
  float redHold;
  bool skipFinal, lead, leadSame;
  float leadSec;
  float holdMs, maxHz, master;
  PinCfg pins[2];              // 0 = timer LED, 1 = set LED
  Profile load, trans, leadP;

  static Rgb hex(uint32_t v) { return Rgb{(float)((v >> 16) & 255), (float)((v >> 8) & 255), (float)(v & 255)}; }

  static Profile transDefaults() {
    Profile p;
    memset(&p, 0, sizeof p);
    p.rate0 = 10; p.rate1 = 150; p.curve = 3; p.duty0 = 60; p.duty1 = 30; p.soft0 = 100; p.soft1 = 0;
    p.glow = 6; p.blend = 40; p.breath = 2; p.tail = 5; p.f0 = 1; p.f1 = 7;
    p.nColors = 3; p.colors[0] = hex(0x2d6bff); p.colors[1] = hex(0x8b4dff); p.colors[2] = hex(0xff3fd8);
    return p;
  }
  void setDefaults() {
    loadSec = 30; transSec = 15; sets = 5; repeat = 4; trackRepeats = false; redHold = 2; skipFinal = true; lead = true; leadSame = true; leadSec = 15;
    holdMs = 0; maxHz = 7; master = 100;
    pins[0] = PinCfg{13, 11, 10, 12, true};    // timer LED: R=13 G=11 B=10, common anode on 12
    pins[1] = PinCfg{2, 4, 5, 3, true};        // set LED:   R=2  G=4  B=5,  common anode on 3
    Profile l;
    memset(&l, 0, sizeof l);
    l.rate0 = 10; l.rate1 = 150; l.curve = 3; l.duty0 = 60; l.duty1 = 30; l.soft0 = 100; l.soft1 = 0;
    l.solid = 3; l.fade = 1; l.finalSec = 30; l.longCycle = 4; l.longDim = 15; l.markEvery = 0; l.glow = 0; l.blend = 30; l.breath = 2; l.tail = 5; l.f0 = 1; l.f1 = 7;
    l.nColors = 4; l.colors[0] = hex(0x22e070); l.colors[1] = hex(0xffd51a); l.colors[2] = hex(0xff7a14); l.colors[3] = hex(0xff1f2d);
    load = l;
    trans = transDefaults();
    leadP = transDefaults();
  }
};

// ---- session plan -------------------------------------------------------------------------------------------------
// lead-in once, then the run of sets `repeat` times. Every repeat has the same windows, so only one cycle is stored and
// windows are found by arithmetic. A transition leads from the last set of one repeat into the first set of the next;
// only the very last set (of the last repeat) can drop its transition ("final transition" off).
enum WinType : uint8_t { W_LOAD = 0, W_HOLD = 1, W_LEAD = 2, W_TRANS = 3 };
struct Win { uint8_t type; uint8_t set; uint8_t rep; double len; double start; };
struct Plan {
  bool hasLead;
  double leadLen;
  Win cyc[MAX_CYC];            // one repeat, start relative to the repeat
  int cycN, lastN;             // windows in a repeat / in the last repeat (one fewer when the final transition is dropped)
  double cycLen, lastLen;
  int reps;
  int n;                       // windows in the whole session
  double total;
};

inline void buildPlan(const Config& c, Plan& p) {
  p.cycN = 0;
  p.hasLead = false; p.leadLen = 0;
  if (c.lead) {
    double len = c.leadSame ? c.transSec : c.leadSec;
    if (len > 0.0) { p.hasLead = true; p.leadLen = len; }
  }
  double start = 0;
  for (int i = 0; i < c.sets; i++) {
    auto push = [&](uint8_t type, double len) {
      if (p.cycN >= MAX_CYC) return;
      p.cyc[p.cycN++] = Win{type, (uint8_t)(i + 1), 1, len, start};
      start += len;
    };
    push(W_LOAD, c.loadSec);
    if (c.redHold > 0.f) push(W_HOLD, c.redHold);                         // red hold after every load, the last one included
    if (c.transSec > 0.f) push(W_TRANS, c.transSec);
  }
  p.cycLen = start;
  p.reps = c.repeat < 1 ? 1 : c.repeat;
  bool dropFinal = c.skipFinal && c.transSec > 0.f && p.cycN > 0;
  p.lastN = dropFinal ? p.cycN - 1 : p.cycN;
  p.lastLen = dropFinal ? p.cycLen - c.transSec : p.cycLen;
  p.n = (p.hasLead ? 1 : 0) + (p.reps - 1) * p.cycN + p.lastN;
  p.total = (p.hasLead ? p.leadLen : 0.0) + (p.reps - 1) * p.cycLen + p.lastLen;
}

inline bool planEqual(const Plan& a, const Plan& b) {
  if (a.hasLead != b.hasLead || a.leadLen != b.leadLen || a.cycN != b.cycN || a.lastN != b.lastN || a.reps != b.reps ||
      a.cycLen != b.cycLen || a.lastLen != b.lastLen)
    return false;
  for (int i = 0; i < a.cycN; i++)
    if (a.cyc[i].type != b.cyc[i].type || a.cyc[i].set != b.cyc[i].set || a.cyc[i].len != b.cyc[i].len || a.cyc[i].start != b.cyc[i].start) return false;
  return true;
}

// The idx-th window of the session, with its absolute start.
inline Win winAt(const Plan& p, int idx) {
  if (p.hasLead) {
    if (idx == 0) return Win{W_LEAD, 0, 1, p.leadLen, 0.0};
    idx--;
  }
  int r = idx / p.cycN;
  if (r > p.reps - 1) r = p.reps - 1;
  Win w = p.cyc[idx - r * p.cycN];
  w.rep = (uint8_t)(r + 1);
  w.start += (p.hasLead ? p.leadLen : 0.0) + r * p.cycLen;
  return w;
}

struct Loc { bool done; int idx; double local; float p; Win win; };
inline Loc locate(const Plan& p, double t) {
  Loc l;
  if (t >= p.total) {
    l.done = true; l.idx = p.n - 1; l.win = winAt(p, l.idx); l.local = l.win.len; l.p = 1.f; return l;
  }
  int idx = 0;
  if (p.hasLead && t < p.leadLen) idx = 0;
  else {
    double tt = t - (p.hasLead ? p.leadLen : 0.0);
    int r = (int)floor(tt / p.cycLen);
    if (r < 0) r = 0;
    if (r > p.reps - 1) r = p.reps - 1;
    double c = tt - r * p.cycLen;
    int lim = (r == p.reps - 1) ? p.lastN : p.cycN, j = 0;
    for (int i = 0; i < lim; i++) { if (c >= p.cyc[i].start) j = i; else break; }
    idx = (p.hasLead ? 1 : 0) + r * p.cycN + j;
  }
  l.done = false; l.idx = idx; l.win = winAt(p, idx);
  l.local = t - l.win.start; l.p = (float)(l.local / l.win.len);
  return l;
}

// ---- set colors: the last three sets are always green, yellow, red; extra sets are prepended -------------------------
inline Rgb extraColor(int i) {
  static const uint32_t EXTRA[5] = {0x2d6bff, 0xff3fd8, 0x19d3ff, 0x8b4dff, 0xffffff};
  if (i < 5) return Config::hex(EXTRA[i]);
  double h = 165.0 + fmod((double)(i - 5) * 0.618, 1.0) * 180.0;     // cyan through pink, away from green, yellow and red
  return hslToRgb((float)(h / 360.0), 1.f, 0.55f);
}
inline void setColors(int N, Rgb* out) {
  static const Rgb tail[3] = {{34.f, 224.f, 112.f}, {255.f, 213.f, 26.f}, {255.f, 31.f, 45.f}};
  for (int i = 0; i < N && i < MAX_SETS; i++) {
    int fromEnd = N - 1 - i;
    out[i] = fromEnd < 3 ? tail[2 - fromEnd] : extraColor(fromEnd - 3);
  }
}

// ---- LED outputs --------------------------------------------------------------------------------------------------
struct Led { Rgb c; float b; };    // color 0..255 per channel, brightness 0..1

// Load window. The last finalSec seconds (30 by default; the whole window when it is shorter) run the accelerating pulse and
// step through the colors. A longer load pulses slowly before that, one color after another, each getting an equal share
// of the time; the final stretch then starts again at the first color.
inline Led pulseLed(const Profile& pr, double len, double local, float maxHz) {
  double F = fmin((double)pr.finalSec, len), head = len - F;
  int K = pr.nColors;
  Rgb c;
  float b;
  if (local < head) {
    double seg = head / (double)K;
    float w = (float)fmin((double)pr.blend / 100.0, 3.0 / seg);                // colors change at once, with at most a 3 s crossfade
    c = gradientAt(pr.colors, K, (float)(local / head), w);
    float dim = pr.longDim / 100.f;
    b = dim + (1.f - dim) * (0.5f + 0.5f * cosf(TAU_F * (float)(fmod(local, (double)pr.longCycle) / (double)pr.longCycle)));   // starts at the peak
  } else {
    double lt = local - head;
    double f0 = fmin((double)pr.rate0 / F, (double)maxHz), f1 = fmin((double)pr.rate1 / F, (double)maxHz);
    Accel a = accelOf(lt, F, f0, f1, pr.curve);
    float x = (float)(a.cyc - floor(a.cyc));
    float w = pulseShape(x, lerpf(pr.duty0, pr.duty1, a.q) / 100.f, lerpf(pr.soft0, pr.soft1, a.q) / 100.f);
    float fl = pr.glow / 100.f;
    b = fl + (1.f - fl) * w;
    c = gradientAt(pr.colors, K, (float)(lt / F), pr.blend / 100.f);
  }
  float S = (float)fmin((double)pr.solid, len * 0.5), Fd = (float)fmin((double)pr.fade, len * 0.25);
  if (local < S) b = 1.f;                                    // solid start, then it fades out into what follows
  else if (local < S + Fd) b = 1.f + (b - 1.f) * smoothf(S, S + Fd, (float)local);
  // time marks (long loads only): a quick double flash every markEvery seconds
  if (pr.markEvery > 0.f && local >= (double)pr.markEvery && local < head) {
    double m = fmod(local, (double)pr.markEvery);
    if (m < 0.5) b = (m < 0.12 || (m >= 0.25 && m < 0.37)) ? 1.f : 0.f;
  }
  return Led{c, b};
}

// Transition / lead-in: slow constant breathing, then the last seconds ramp from a slow to a fast pulse.
inline Led transLed(const Profile& pr, double len, double local) {
  double tail = fmin((double)pr.tail, len), ts = len - tail;
  float fl = pr.glow / 100.f, w;
  Rgb c = gradientAt(pr.colors, pr.nColors, (float)(local / len), pr.blend / 100.f);
  if (local < ts) {
    float ph = (float)(fmod(local, (double)pr.breath) / (double)pr.breath);
    w = 0.5f + 0.5f * cosf(TAU_F * ph);                       // starts at the peak, so it flows out of the start flash
  } else {
    Accel a = accelOf(local - ts, tail, pr.f0, pr.f1, pr.curve);
    float x = (float)(a.cyc - floor(a.cyc));
    w = pulseShape(x, lerpf(pr.duty0, pr.duty1, a.q) / 100.f, lerpf(pr.soft0, pr.soft1, a.q) / 100.f);
  }
  return Led{c, fl + (1.f - fl) * w};
}

inline const Profile& profileFor(const Config& c, uint8_t type) {
  if (type == W_LOAD) return c.load;
  if (type == W_LEAD) return c.leadSame ? c.trans : c.leadP;
  return c.trans;
}
inline Rgb firstColor(const Config& c, uint8_t type) {
  if (type == W_HOLD) return HOLD_COL;
  return profileFor(c, type).colors[0];
}
inline Led led1(const Config& c, const Win& w, double local) {
  Led out;
  if (w.type == W_HOLD) out = Led{HOLD_COL, 1.f};
  else if (w.type == W_LOAD) out = pulseLed(c.load, (double)w.len, local, c.maxHz);
  else out = transLed(profileFor(c, w.type), (double)w.len, local);
  float hold = c.holdMs / 1000.f;
  if (hold > 0.f && local < fmin((double)hold, (double)w.len * 0.25)) out = Led{firstColor(c, w.type), 1.f};   // solid flash marks a window start
  return out;
}
// The set (and repeat) a window leads into; LED 2 shows the set's color, so during a transition or lead-in it is the upcoming set.
struct SetRep { int set, rep; };
inline SetRep nextOf(const Config& c, const Win& w) {
  if (w.type == W_LEAD) return SetRep{1, 1};
  if (w.type != W_TRANS) return SetRep{w.set, w.rep};
  if (w.set >= c.sets) return SetRep{1, w.rep + 1 > c.repeat ? c.repeat : w.rep + 1};      // wraps into the next repeat
  return SetRep{w.set + 1, w.rep};
}
inline int setOf(const Config& c, const Win& w) { return nextOf(c, w).set; }
// How many colors LED 2 steps through: one per set, or one per repeat.
inline int trackN(const Config& c) { return c.trackRepeats ? c.repeat : c.sets; }
inline Led led2(const Config& c, const Win& w, const Rgb* setCols) {
  SetRep n = nextOf(c, w);
  return Led{setCols[(c.trackRepeats ? n.rep : n.set) - 1], 0.85f};
}

// ---- timer state machine ------------------------------------------------------------------------------------------
enum State : uint8_t { ST_IDLE = 0, ST_RUN, ST_PAUSE, ST_PREVIEW, ST_DONE, ST_STOPPED };
inline const char* stateName(State s) {
  switch (s) {
    case ST_IDLE: return "idle";
    case ST_RUN: return "run";
    case ST_PAUSE: return "pause";
    case ST_PREVIEW: return "preview";
    case ST_DONE: return "done";
    default: return "stopped";
  }
}

struct Out { Rgb c1; float b1; Rgb c2; float b2; };

class Engine {
 public:
  Config cfg;
  Plan plan;
  Rgb setCols[MAX_SETS];
  State state;

  Engine() : state(ST_IDLE), baseT_(0), runUs_(0) {
    cfg.setDefaults();
    buildPlan(cfg, plan);
    setColors(trackN(cfg), setCols);
  }

  // Take new settings. A changed plan (timing, lead-in, hold, final transition) resets the timer; style changes don't.
  void configure(const Config& c) {
    Plan np;
    buildPlan(c, np);
    bool same = planEqual(plan, np);
    cfg = c;
    plan = np;
    setColors(trackN(cfg), setCols);
    if (!same) reset();
    else if (baseT_ > plan.total - 0.001) baseT_ = plan.total - 0.001;
  }

  double position(int64_t nowUs) const { return state == ST_RUN ? baseT_ + (double)(nowUs - runUs_) * 1e-6 : baseT_; }

  void start(int64_t nowUs, bool haveT, double t) {
    double pos = haveT ? t : position(nowUs);
    if (state == ST_DONE || state == ST_STOPPED || pos >= plan.total) pos = 0;
    baseT_ = clampd(pos, 0.0, plan.total - 0.001);
    runUs_ = nowUs;
    state = ST_RUN;
  }
  void pause(int64_t nowUs) { if (state == ST_RUN) { baseT_ = position(nowUs); state = ST_PAUSE; } }
  void stop(int64_t nowUs) {
    if (state == ST_RUN || state == ST_PAUSE || (state == ST_PREVIEW && baseT_ > 0)) { baseT_ = position(nowUs); state = ST_STOPPED; }
  }
  void reset() { state = ST_IDLE; baseT_ = 0; }
  void seek(int64_t nowUs, double t) {
    baseT_ = clampd(t, 0.0, plan.total - 0.001);
    runUs_ = nowUs;
    if (state != ST_RUN) state = ST_PREVIEW;
  }

  // Advance the state and compute both LEDs. Brightness is 0..1 before the master level.
  void render(int64_t nowUs, Out& o) {
    double nowS = (double)nowUs * 1e-6;
    double pos = position(nowUs);
    if (state == ST_RUN && pos >= plan.total) { state = ST_DONE; baseT_ = plan.total; pos = plan.total; }
    Loc loc = locate(plan, pos);
    const Win& win = loc.win;
    switch (state) {
      case ST_IDLE: {
        float br = (float)(0.14 + 0.1 * sin(nowS * TAU_D * 0.3));
        o.c1 = firstColor(cfg, plan.hasLead ? W_LEAD : W_LOAD); o.b1 = br;
        o.c2 = Rgb{226.f, 236.f, 255.f}; o.b2 = br * 0.8f;
        break;
      }
      case ST_RUN: {
        Led a = led1(cfg, win, loc.local), b = led2(cfg, win, setCols);
        o.c1 = a.c; o.b1 = a.b; o.c2 = b.c; o.b2 = b.b;
        break;
      }
      case ST_PAUSE: {
        Led a = led1(cfg, win, loc.local);
        o.c1 = a.c; o.b1 = 0.45f;
        o.c2 = Rgb{255.f, 170.f, 0.f}; o.b2 = fmod(nowS, 1.2) < 0.6 ? 0.9f : 0.06f;
        break;
      }
      case ST_PREVIEW: {                         // loops a 2.5 s slice at the scrub position
        double span = fmax(0.0001, fmin((double)win.len - loc.local, 2.5));
        double lt = loc.local + (fmod(nowS, 2.5) / 2.5) * span;
        Led a = led1(cfg, win, lt), b = led2(cfg, win, setCols);
        o.c1 = a.c; o.b1 = a.b; o.c2 = b.c; o.b2 = b.b;
        break;
      }
      case ST_STOPPED:
        o.c1 = Rgb{255.f, 255.f, 255.f}; o.b1 = 0.f; o.c2 = o.c1; o.b2 = 0.f;
        break;
      default:                                   // done: slow rainbow and a breathing white
        o.c1 = hslToRgb((float)fmod(nowS * 0.12, 1.0), 1.f, 0.55f); o.b1 = 0.9f;
        o.c2 = Rgb{255.f, 255.f, 255.f}; o.b2 = (float)(0.6 + 0.3 * sin(nowS * TAU_D * 0.4));
        break;
    }
  }

 private:
  double baseT_;
  int64_t runUs_;
};

}  // namespace bc
