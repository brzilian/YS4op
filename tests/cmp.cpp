// Renders ROM voices through fm4's opz.c and through YS4OP's opz_engine.h port; compares samples.
#include <cstdio>
#include <cmath>
#include <vector>
extern "C" {
#include "opz.h"
}
#include "opz_engine.h"
#include "rombank.h"

static void toFm4(const ys::Patch& p, opz_patch& q)
{
    for (int i = 0; i < 4; ++i) {
        const ys::OpParams& o = p.op[i]; opz_op_params& d = q.op[i];
        d.ar = o.ar; d.d1r = o.d1r; d.d2r = o.d2r; d.rr = o.rr; d.d1l = o.d1l; d.ls = o.ls; d.rs = o.rs; d.ame = o.ame; d.kvs = o.kvs; d.out = o.out;
        d.crs = o.crs; d.det = o.det; d.fixed = o.fix; d.fixrg = o.fixrg; d.fine = o.fine; d.wave = o.osw; d.shft = o.egsft; d.on = 1;
    }
    q.alg = p.alg; q.fb = p.fbl; q.lfs = p.lfs; q.lfd = p.lfd; q.pmd = p.pmd; q.amd = p.amd; q.sync = p.sync; q.lfw = p.lfw; q.pms = p.pms; q.ams = p.ams; q.rev = p.rev;
    q.transpose = (int8_t)(p.trps - 24);
}
int main()
{
    double worst = 0; int worstVoice = -1;
    for (int vi = 0; vi < 100; ++vi) {
        ys::Patch p; ys::unpackVmem(kRomBank[vi], p);
        static opz_engine A; opz_init(&A, 16);
        opz_patch q = {}; toFm4(p, q);
        opz_lfo_setup(&A, 0, &q); opz_lfo_keyon(&A, 0);
        const int note = 60, key = note + (p.trps - 24);
        opz_key_on(&A, 0, &q, 0, (unsigned)key, 100, 128, 128, 0);   // gain 128 >> 7 = x1
        static opz::Engine B; B.prepare(44100.0); B.lfoSetup(p); B.lfoKeyOn();
        B.keyOn(0, p, 15, note, 100, false);
        double maxd = 0, peak = 0;
        const int blocks = 2 * 44100 / 128;
        for (int b = 0; b < blocks; ++b) {
            if (b == blocks / 2) { opz_key_off(&A, 0); B.keyOff(0); }
            int32_t dry[256] = {0}, send[256] = {0};
            opz_render(&A, dry, send);
            float out[128] = {0};
            B.control(128);            // fm4 updates LFO + pitch once per 128-frame block
            B.render(out, 128);
            for (int s = 0; s < 128; ++s) {
                const double a = dry[2 * s] / 8192.0, c = out[s];
                maxd = std::fmax(maxd, std::fabs(a - c)); peak = std::fmax(peak, std::fabs(a));
            }
        }
        if (maxd > worst) { worst = maxd; worstVoice = vi; }
        if (vi < 5 || maxd > 1e-6) std::printf("voice %2d %.10s  peak %.3f  max diff %.2e\n", vi, p.name, peak, maxd);
    }
    std::printf("WORST max abs diff over 100 voices: %.3e (voice %d)\n", worst, worstVoice);
}
