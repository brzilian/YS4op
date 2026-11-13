#include <initializer_list>
#include <cstdio>
#include <cmath>
#include "opz_engine.h"
int main() {
  for (double sr : {44100.0, 48000.0}) {
    ys::Patch p; p.alg = 7; for (auto& o : p.op) { o.out = 0; o.ar = 31; o.d1l = 15; o.rr = 15; o.crs = 4; o.det = 3; }
    p.op[0].out = 99;                       // OP1 sine carrier, ratio 1.00
    p.op[1].out = 99; p.op[1].fix = 1; p.op[1].crs = 4 * 4; p.op[1].fine = 0; p.op[1].fixrg = 3;   // fixed 64*8 = 512 Hz
    for (int test = 0; test < 2; ++test) {
      ys::Patch q = p; if (test == 0) q.op[1].out = 0; else q.op[0].out = 0;
      static opz::Engine e; e.prepare(sr); e.lfoSetup(q); e.keyOn(0, q, 15, 69, 127, false);
      const int N = (int)sr * 2; static float buf[200000]; for (int i = 0; i < N; ++i) buf[i] = 0; for (int i = 0; i < N; i += 32) { e.control(32); e.render(buf + i, 32); }
      // count rising zero crossings over the last second with linear interpolation
      double first = -1, last = -1; int cnt = 0;
      for (int i = N / 2; i < N - 1; ++i) if (buf[i] <= 0 && buf[i + 1] > 0) { double t = i + buf[i] / (buf[i] - buf[i + 1]); if (first < 0) first = t; last = t; ++cnt; }
      std::printf("sr %.0f  %s: %.3f Hz\n", sr, test == 0 ? "A4 ratio 1.00" : "fixed 512 Hz", (cnt - 1) / ((last - first) / sr));
    }
  }
}
