// Host test for engine.h / config.h / json.h:  g++ -std=c++11 -O1 -I.. host_test.cpp -o host_test
//   ./host_test dump config.json   -> prints plan, set colors and LED samples (compared against the web UI's JS engine)
//   ./host_test state              -> checks the timer state machine
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "engine.h"
#include "config.h"

using namespace bc;

static std::string slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
  std::string s; char buf[4096]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static int dump(const char* path) {
  std::string js = slurp(path);
  Config c; bool pinsOk = true;
  if (!cfgFromJson(js.c_str(), js.size(), c, &pinsOk)) { printf("PARSE FAIL\n"); return 1; }
  Plan pl; buildPlan(c, pl);
  Rgb sc[MAX_SETS]; setColors(c.sets, sc);
  printf("PINS %d %d,%d,%d,%d,%d %d,%d,%d,%d,%d\n", pinsOk ? 1 : 0, c.pins[0].r, c.pins[0].g, c.pins[0].b, c.pins[0].com, c.pins[0].anode, c.pins[1].r, c.pins[1].g, c.pins[1].b, c.pins[1].com, c.pins[1].anode);
  printf("PLAN %d %.3f\n", pl.n, pl.total);
  for (int i = 0; i < pl.n; i++) { Win w = winAt(pl, i); printf("W %d %d %d %d %.3f %.3f\n", i, w.type, w.set, w.rep, w.len, w.start); }
  for (int i = 0; i < c.sets; i++) printf("C %d %.3f %.3f %.3f\n", i, sc[i].r, sc[i].g, sc[i].b);
  for (int i = 0; i < pl.n; i++) {
    Win w = winAt(pl, i);
    for (int k = 0; k < 64; k++) {
      double local = (double)w.len * (k + 0.5) / 64.0;
      Led a = led1(c, w, local), b = led2(c, w, sc);
      printf("S %d %d %.4f %.4f %.4f %.5f %.3f %.3f %.3f %.3f\n", i, k, a.c.r, a.c.g, a.c.b, a.b, b.c.r, b.c.g, b.c.b, b.b);
    }
  }
  return 0;
}

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)
static int64_t S(double sec) { return (int64_t)(sec * 1e6); }

static int stateTest() {
  Engine e;                                   // defaults: lead 15, 5 sets x 4 repeats, total 940
  CHECK(fabs(e.plan.total - 940.0) < 1e-3);
  CHECK(e.state == ST_IDLE);
  Out o;
  e.render(S(1), o);
  CHECK(o.b1 > 0.03f && o.b1 < 0.3f);         // idle breathing

  e.start(S(10), false, 0);                   // start from 0 at wall time 10 s
  CHECK(e.state == ST_RUN);
  CHECK(fabs(e.position(S(20)) - 10.0) < 1e-6);
  e.pause(S(25));
  CHECK(e.state == ST_PAUSE && fabs(e.position(S(99)) - 15.0) < 1e-6);
  e.start(S(100), true, 15.0);                // resume from the same spot
  CHECK(fabs(e.position(S(105)) - 20.0) < 1e-6);
  e.stop(S(110));
  CHECK(e.state == ST_STOPPED && fabs(e.position(S(500)) - 25.0) < 1e-6);
  e.render(S(111), o);
  CHECK(o.b1 == 0.f && o.b2 == 0.f);          // LEDs off when stopped
  e.start(S(200), false, 0);                  // Start after Stop restarts from 0
  CHECK(e.state == ST_RUN && fabs(e.position(S(201)) - 1.0) < 1e-6);
  e.reset();
  CHECK(e.state == ST_IDLE && e.position(S(300)) == 0.0);

  e.seek(S(300), 60.0);                       // scrub: preview a slice
  CHECK(e.state == ST_PREVIEW && fabs(e.position(S(301)) - 60.0) < 1e-6);
  e.render(S(301), o);
  CHECK(o.b2 > 0.8f);                         // set LED solid
  e.start(S(310), true, 60.0);
  e.seek(S(320), 100.0);                      // seeking while running keeps running
  CHECK(e.state == ST_RUN && fabs(e.position(S(325)) - 105.0) < 1e-6);

  e.start(S(400), true, 0.0);
  e.render(S(400 + 940.5), o);                // past the end
  CHECK(e.state == ST_DONE);
  CHECK(fabs(e.position(S(1999)) - 940.0) < 1e-6);
  e.start(S(2500), false, 0);                 // Start after done restarts
  CHECK(e.state == ST_RUN && e.position(S(2500)) == 0.0);

  // window identity: red hold after the last load, nothing after it
  const Plan& p = e.plan;
  CHECK(winAt(p, p.n - 1).type == W_HOLD && winAt(p, p.n - 2).type == W_LOAD);
  CHECK(winAt(p, 0).type == W_LEAD && winAt(p, 0).len == 15.0);
  CHECK(p.n == 1 + 3 * 15 + 14);
  e.start(S(2000), true, winAt(p, p.n - 1).start + 0.5);
  e.render(S(2000), o);
  CHECK(o.c1.r == 255.f && o.c1.g == 31.f && o.b1 == 1.f);   // solid red during the last hold

  // a changed plan resets, a style change does not
  Config c = e.cfg;
  c.load.duty0 = 50;
  e.start(S(3000), true, 40.0);
  e.configure(c);
  CHECK(e.state == ST_RUN);
  c.loadSec = 20;
  e.configure(c);
  CHECK(e.state == ST_IDLE && fabs(e.plan.total - (15 + 3 * 185 + 170)) < 1e-3);

  // repeat: lead-in once, transition from the last set into the next repeat, last set of the last repeat ends the session
  {
    Config rc; rc.setDefaults();
    Plan rp;
    buildPlan(rc, rp);
    CHECK(rp.reps == 4 && rp.cycN == 15 && rp.lastN == 14);
    Loc a = locate(rp, 15.0 + 235.0 - 1.0);                      // last second of the first repeat: its closing transition
    CHECK(a.win.type == W_TRANS && a.win.set == 5 && a.win.rep == 1 && !a.done);
    SetRep n1 = nextOf(rc, a.win);
    CHECK(n1.set == 1 && n1.rep == 2);
    Loc b = locate(rp, 15.0 + 235.0 + 0.5);                      // first load of the second repeat
    CHECK(b.win.type == W_LOAD && b.win.set == 1 && b.win.rep == 2 && fabs(b.local - 0.5) < 1e-9);
    CHECK(b.idx == 1 + 15);
    Loc z = locate(rp, rp.total - 0.25);                         // the end: last hold of repeat 4
    CHECK(z.win.type == W_HOLD && z.win.set == 5 && z.win.rep == 4 && z.idx == rp.n - 1);
    CHECK(locate(rp, rp.total).done);
    Config r1 = rc; r1.repeat = 1;                               // no repeat: the old single run
    buildPlan(r1, rp);
    CHECK(fabs(rp.total - (15 + 5 * 47 - 15)) < 1e-9 && rp.n == 1 + 14);
    CHECK(nextOf(r1, winAt(rp, 3 * 3 + 3 - 3)).rep == 1);
    r1.skipFinal = false; buildPlan(r1, rp);                     // final transition on
    CHECK(fabs(rp.total - (15 + 5 * 47)) < 1e-9 && winAt(rp, rp.n - 1).type == W_TRANS);
    Config r3 = rc; r3.transSec = 0; r3.lead = false; r3.redHold = 0; r3.repeat = 3; buildPlan(r3, rp);   // bare loads only
    CHECK(rp.n == 15 && fabs(rp.total - 15 * 30) < 1e-9 && winAt(rp, 7).rep == 2 && winAt(rp, 7).set == 3);
  }

  // load window start: 3 s solid first color, 1 s fade out, then the pulse train
  {
    Config lc; lc.setDefaults();
    Led x = led1(lc, Win{W_LOAD, 1, 1, 30.0, 0.0}, 1.0);
    CHECK(x.b == 1.f && x.c.g == 224.f && x.c.r == 34.f);
    x = led1(lc, Win{W_LOAD, 1, 1, 30.0, 0.0}, 2.999);
    CHECK(x.b == 1.f);
    Led y = led1(lc, Win{W_LOAD, 1, 1, 30.0, 0.0}, 3.5);
    CHECK(y.b > 0.f && y.b < 1.f);                                // mid fade
    Led z = led1(lc, Win{W_LOAD, 1, 1, 30.0, 0.0}, 4.0);
    CHECK(fabsf(z.b - pulseLed(lc.load, 30.0, 4.0, lc.maxHz).b) < 1e-6f && z.b < 1.f);   // fade done: exactly the pulse train
    Led tr = led1(lc, Win{W_TRANS, 1, 1, 15.0, 0.0}, 1.0);
    CHECK(tr.b < 1.f || tr.c.r != 34.f);                          // transitions have no solid start
    Config sc = lc; sc.load.solid = 0; sc.load.fade = 0;          // switched off: pulses from the first moment
    CHECK(fabsf(led1(sc, Win{W_LOAD, 1, 1, 30.0, 0.0}, 1.0).b - pulseLed(sc.load, 30.0, 1.0, sc.maxHz).b) < 1e-6f);
  }

  // settings parser: defaults on {}, clamping, bad pins, bad json
  Config d; bool ok = true;
  CHECK(cfgFromJson("{}", 2, d, &ok) && ok && d.pins[0].r == 13 && d.pins[1].com == 3 && d.sets == 5 && d.repeat == 4);
  const char* j1 = "{\"sets\":99,\"loadSec\":1,\"pins\":{\"timer\":{\"r\":13,\"g\":13,\"b\":10,\"com\":12,\"anode\":false}}}";
  CHECK(cfgFromJson(j1, strlen(j1), d, &ok));
  CHECK(!ok && d.sets == 20 && d.repeat == 4 && d.loadSec == 3.f && d.pins[0].g == 11 && d.pins[0].anode == false);
  { const char* j3 = "{\"repeat\":99}"; Config q; bool ok2 = true; CHECK(cfgFromJson(j3, strlen(j3), q, &ok2) && q.repeat == 20); j3 = "{\"repeat\":0}"; CHECK(cfgFromJson(j3, strlen(j3), q, &ok2) && q.repeat == 1); }
  const char* j2 = "{\"pins\":{\"set\":{\"r\":7,\"g\":8,\"b\":9,\"com\":-1,\"anode\":true}},\"profiles\":{\"load\":{\"colors\":[\"#ff0000\",\"#00ff00\"]}}}";
  CHECK(cfgFromJson(j2, strlen(j2), d, &ok) && ok && d.pins[1].com == -1 && d.pins[1].r == 7 && d.load.nColors == 2 && d.load.colors[1].g == 255.f);
  const char* j3 = "{\"sets\":";
  CHECK(!cfgFromJson(j3, strlen(j3), d, &ok));
  const char* j4 = "{\"pins\":{\"timer\":{\"r\":19}}}";      // USB pin is refused
  CHECK(cfgFromJson(j4, strlen(j4), d, &ok) && !ok && d.pins[0].r == 13);

  printf(fails ? "STATE TEST: %d FAILED\n" : "STATE TEST OK\n", fails);
  return fails ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 3 && !strcmp(argv[1], "dump")) return dump(argv[2]);
  if (argc >= 2 && !strcmp(argv[1], "state")) return stateTest();
  fprintf(stderr, "usage: host_test dump config.json | state\n");
  return 2;
}
