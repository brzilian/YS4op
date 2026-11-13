// opz_engine.h - YM2414 (OPZ) 4-operator voice engine for YS4OP.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from the FM-4 firmware's OPZ engine (github.com/brzilian/fm4, firmware/app/fm4/opz.c,
// GPL-3.0-only), whose chip-level behaviour follows ymfm by Aaron Giles (BSD-3-Clause, tables in
// opz_ymfm_tables.h): log-sine / exp tables and 13-bit operator output, the chip envelope
// increment tables and EG clock (chip rate / 3), keycode phase steps with DT1/DT2, the TX81Z
// CRS -> MUL/DT2 map, the OPZ LFO counter and PMS/AMS shifts, EG shift and the REV tail stage.
//
// Differences from the firmware engine (behaviour at 44.1 kHz is unchanged):
//   * runs at any sample rate: phase steps, EG clock and LFO counter are rescaled from the chip's
//     55.93 kHz in prepare(), instead of being fixed 44.1 kHz constants;
//   * any render length (MPC blocks are 32-512 frames) instead of fixed 128-frame blocks;
//   * one LFO (single-timbral plugin) with the firmware's MW/BC/AT/FC depth additions;
//   * float output: one carrier at full level = 1.0 (chip 13-bit full scale).

#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include "opz_ymfm_tables.h"
#include "ys_patch.h"

namespace opz {

constexpr int kMaxVoices = 16;
constexpr uint16_t kEgQuiet = 0x380;
constexpr double kChipRate = 3579545.0 / 64.0;     // 55930.4 Hz
enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE, EG_REVERB };

// TQ5/TX81Z CRS 0..63 -> chip MUL (0..15) and DT2 (0..3), ascending ratio.
static const uint8_t kCrsMap[64][2] = {
    {0,0},{0,1},{0,2},{0,3},{1,0},{1,1},{1,2},{1,3},{2,0},{2,1},{3,0},{2,2},{2,3},{4,0},{3,1},{3,2},
    {5,0},{3,3},{4,1},{6,0},{4,2},{4,3},{7,0},{5,1},{5,2},{8,0},{6,1},{5,3},{9,0},{6,2},{7,1},{10,0},
    {6,3},{7,2},{11,0},{8,1},{12,0},{7,3},{8,2},{9,1},{13,0},{8,3},{14,0},{9,2},{10,1},{15,0},{11,1},{9,3},
    {10,2},{12,1},{11,2},{10,3},{13,1},{12,2},{11,3},{14,1},{13,2},{12,3},{15,1},{14,2},{13,3},{15,2},{14,3},{15,3}
};
static const int16_t kDt2Delta[4] = { 0, 384, 500, 608 };          // 1/64 semitone (0, 600, 781, 950 cents)
static const uint8_t kOutLow[20] = { 127,122,118,114,110,107,104,102,100,98,96,94,92,90,88,86,85,84,82,81 };
// Algorithm routing, ymfm order: O1 = OP4 (feedback) .. O4 = OP1.
// inputs: 0 none, 1 O1, 2 O2, 3 O3, 5 O1+O2, 6 O1+O3, 7 O2+O3 ; fields: O2 in, O3 in, O4 in, O1 out, O2 out, O3 out
static constexpr uint8_t kAlg[8][6] = {
    {1,2,3, 0,0,0}, {0,5,3, 0,0,0}, {0,2,6, 0,0,0}, {1,0,7, 0,0,0},
    {1,0,3, 0,1,0}, {1,1,1, 0,1,1}, {1,0,0, 0,1,1}, {0,0,0, 1,1,1},
};
// PMS 0..7 depth in cents at full PMD (for the Easy Edit Vibrato mapping), AMS 0..3 in dB
static const float kPmsCents[8] = {0, 5, 10, 20, 50, 100, 400, 700};
static const float kAmsDb[4] = {0.0f, 23.9f, 47.8f, 95.6f};

struct Waves
{
    uint16_t w[8][1024];   // 4.8 log attenuation, bit 15 = sign
    bool ready = false;
    static uint16_t absSin(uint32_t in) { if (in & 0x100) in = ~in; return opz_sin_table[in & 0xff]; }
    void init()
    {
        if (ready) return;
        for (unsigned i = 0; i < 1024; ++i) w[0][i] = (uint16_t)(absSin(i) | (((i >> 9) & 1) << 15));
        const uint16_t zero = w[0][0];
        for (unsigned i = 0; i < 1024; ++i)
        {
            const uint16_t sq = (uint16_t)(2U * (w[0][i] & 0x7fffU));
            w[1][i] = (uint16_t)((sq < zero ? sq : zero) | (((i >> 9) & 1) << 15));
        }
        for (unsigned i = 0; i < 1024; ++i)
        {
            const unsigned hi = (i >> 9) & 1;
            w[2][i] = hi ? zero : w[0][i];
            w[3][i] = hi ? zero : w[1][i];
            w[4][i] = hi ? zero : w[0][(i * 2) & 1023];
            w[5][i] = hi ? zero : w[1][(i * 2) & 1023];
            w[6][i] = hi ? zero : w[0][(i * 2) & 0x1ff];
            w[7][i] = hi ? zero : w[1][(i * 2) & 0x1ff];
        }
        ready = true;
    }
};
inline Waves& waves() { static Waves t; return t; }

struct Op
{
    uint32_t phase = 0, inc = 0, fixedInc = 0;
    uint16_t env = 0x3ff, sustain = 0, tl = 0x3ff;
    uint8_t state = EG_RELEASE, shift = 0, wave = 0, ame = 0, fixed = 0, multiple = 16, det1 = 0, dt2 = 0;
    uint8_t rate[5] = {0, 0, 0, 0, 0};
};

struct Voice
{
    Op o[4];                 // chip order: o[0] = OP4 (feedback) .. o[3] = OP1
    int32_t fb[2] = {0, 0};
    uint8_t alg = 0, fbl = 0, pms = 0, ams = 0;
    bool active = false, keyed = false;
    int note = -1;           // played MIDI note (before transpose)
    int vel = 100;
    int32_t pitch64 = 0;     // transposed note pitch in 1/64 semitone
};

class Engine
{
public:
    Voice v[kMaxVoices];

    void prepare(double sr)
    {
        waves().init();
        sr_ = sr;
        // fm4 calibrates 5196 at 44.1 kHz (A4 = 440.00 Hz); scale for other rates.
        stepScale_ = 5196.0 * 44100.0 / sr;
        egInc_ = (uint32_t)(55930.0 / 3.0 / sr * 65536.0 + 0.5);              // same rounding as fm4
        lfoX64Per128_ = (uint32_t)(55930.0 / sr * 128.0 * 64.0 + 0.5);         // fm4: 10390 at 44.1 kHz
        fixedIncPerHz_ = (uint32_t)(4294967296.0 / sr + 0.5);                   // fm4: 97392 at 44.1 kHz
        reset();
    }

    void reset()
    {
        for (auto& vc : v) vc = Voice();
        egAcc_ = 0; egCounter_ = 0;
        lfoCounter_ = 0; lfoDelayLeft_ = 0; shValue_ = 0; lastIndex_ = 0; noise_ = 0x1234567U;
        pmRaw_ = 0; amRaw_ = 0; extraPm_ = extraAm_ = 0;
    }

    // ---- notes -------------------------------------------------------------------------------
    // note: played key; transpose (TRPS-24) is applied here. legato: change pitch only.
    void keyOn(int slot, const ys::Patch& p, uint8_t opMask, int note, int velocity, bool legato)
    {
        Voice& vc = v[slot];
        const int key = note + ((int)p.trps - 24);
        if (legato && vc.active && vc.keyed) { vc.note = note; vc.pitch64 = clampPitch(key * 64); updatePitch(slot); return; }
        vc.alg = p.alg & 7; vc.fbl = p.fbl & 7; vc.pms = p.pms & 7; vc.ams = p.ams & 3;
        vc.note = note; vc.vel = velocity;
        vc.pitch64 = clampPitch(key * 64);
        vc.fb[0] = vc.fb[1] = 0;
        int vel = velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
        unsigned kc; (void)pitchStep(vc.pitch64, &kc);
        const unsigned noteForLs = key < 0 ? 0u : (unsigned)key;
        for (unsigned i = 0; i < 4; ++i)
        {
            const ys::OpParams& q = p.op[3 - i];      // chip slot i (0 = OP4) <- operator 4 - i
            Op& o = vc.o[i];
            const unsigned mul = kCrsMap[q.crs & 63][0];
            o.dt2 = kCrsMap[q.crs & 63][1];
            unsigned fine = q.fine & 15;
            if ((q.crs & 63) < 4 && fine > 7) fine = 7;      // TX81Z: FINE 0-7 when CRS < 4
            o.multiple = (uint8_t)(mul ? ((mul << 4) | fine) : (8U + fine));
            o.fixed = q.fix ? 1 : 0;
            if (o.fixed) o.fixedInc = (uint32_t)fixedHz(q.crs, q.fixrg, q.fine) * fixedIncPerHz_;
            const int d = (int)q.det - 3;
            o.det1 = (uint8_t)(d < 0 ? (4 | (unsigned)(-d)) : (unsigned)d);
            o.wave = q.osw & 7;
            o.ame = q.ame ? 1 : 0;
            o.shift = i == 3 ? 0 : (q.egsft & 3);         // OP1 has no EG shift on TX81Z/TQ5
            // total level: output level + velocity + keyboard level scaling (attenuation units 0.09375 dB)
            const unsigned out = q.out > 99 ? 99 : q.out;
            const unsigned tl = out >= 20 ? 99U - out : kOutLow[out];
            unsigned att = tl << 3;
            att += ((unsigned)(q.kvs & 7) * (127U - (unsigned)vel) * 3U) >> 3;
            if (noteForLs > 24) att += ((unsigned)q.ls * (noteForLs - 24U)) >> 3;
            if (!((opMask >> (3 - i)) & 1)) att = 0x3ff;
            o.tl = (uint16_t)(att > 0x3ff ? 0x3ff : att);
            const unsigned d1l = 15U - (q.d1l & 15);
            o.sustain = (uint16_t)((d1l | ((d1l + 1) & 0x10)) << 5);
            const unsigned ksr = kc >> ((q.rs & 3) ^ 3);
            o.rate[EG_ATTACK] = effRate((q.ar & 31) * 2U, ksr);
            o.rate[EG_DECAY] = effRate((q.d1r & 31) * 2U, ksr);
            o.rate[EG_SUSTAIN] = effRate((q.d2r & 31) * 2U, ksr);
            unsigned rr = q.rr & 15; if (!rr) rr = 1;
            o.rate[EG_RELEASE] = effRate(rr * 4U + 2U, ksr);
            o.rate[EG_REVERB] = o.rate[EG_RELEASE];
            if (p.rev & 7)
            {
                const uint8_t r = effRate((p.rev & 7) * 4U + 2U, ksr);
                if (r < o.rate[EG_REVERB]) o.rate[EG_REVERB] = r;
            }
            o.phase = 0;
            o.state = EG_ATTACK;
            if (o.rate[EG_ATTACK] >= 62) o.env = 0;
            else if (!vc.active) o.env = 0x3ff;
        }
        vc.active = true; vc.keyed = true;
        updatePitch(slot);
    }

    void keyOff(int slot)
    {
        Voice& vc = v[slot];
        if (!vc.keyed) return;
        vc.keyed = false;
        for (auto& o : vc.o) if (o.state < EG_RELEASE) o.state = EG_RELEASE;
    }

    // ~10 ms click-free release for stolen voices
    void fastRelease(int slot)
    {
        Voice& vc = v[slot];
        if (!vc.active) return;
        vc.keyed = false;
        for (auto& o : vc.o) { o.rate[EG_RELEASE] = o.rate[EG_REVERB] = 60; if (o.state < EG_RELEASE) o.state = EG_RELEASE; }
    }

    void kill(int slot) { v[slot].active = false; v[slot].keyed = false; for (auto& o : v[slot].o) o.env = 0x3ff; }

    void setBend64(int b) { bend64_ = b; }

    // ---- LFO ---------------------------------------------------------------------------------
    void lfoSetup(const ys::Patch& p)
    {
        const unsigned s = p.lfs > 99 ? 99 : p.lfs;
        lfoRate_ = (uint8_t)(0x30U + (s * (0xFFU - 0x30U) + 49U) / 99U);
        lfoWave_ = p.lfw & 3;
        lfoPmd_ = (uint8_t)((p.pmd > 99 ? 99 : p.pmd) * 127U / 99U);
        lfoAmd_ = (uint8_t)((p.amd > 99 ? 99 : p.amd) * 127U / 99U);
        lfoSync_ = p.sync != 0;
        // firmware: LFD x 18 blocks of 128 frames at 44.1 kHz (about 5.2 s at 99)
        lfoDelayTotal_ = (uint32_t)((p.lfd > 99 ? 99 : p.lfd) * 18.0 * 128.0 / 44100.0 * sr_);
    }
    void lfoKeyOn() { if (lfoSync_) lfoCounter_ = 0; lfoDelayLeft_ = lfoDelayTotal_; }
    void lfoExtra(unsigned pm, unsigned am) { extraPm_ = pm > 127 ? 127 : pm; extraAm_ = am > 127 ? 127 : am; }

    // Control-rate update covering the next n samples: LFO and voice pitches.
    void control(int n)
    {
        const uint32_t step = (0x10U | (lfoRate_ & 15U)) << (lfoRate_ >> 4);
        // fm4 advances the chip LFO counter once per 128-frame block by step * 10390 / 64; the same
        // integer arithmetic scaled to n frames (identical to the firmware when n = 128)
        lfoCounter_ += (uint32_t)(((uint64_t)step * lfoX64Per128_ * (uint64_t)n / 128U) >> 6);
        const unsigned idx = (lfoCounter_ >> 22) & 0xff;
        unsigned am, pm;
        if (lfoWave_ == 0) { am = idx ^ 0xff; pm = idx; }                                  // saw
        else if (lfoWave_ == 1) { am = (idx & 0x80) ? 0 : 0xff; pm = am ^ 0x80; }           // square
        else if (lfoWave_ == 2) { am = ((idx & 0x80) ? idx : idx ^ 0xff) << 1 & 0xff; pm = (idx & 0x40) ? am : (~am & 0xff); } // triangle
        else
        {
            if ((idx >> 4) != (lastIndex_ >> 4)) { noise_ = noise_ * 1664525U + 1013904223U; shValue_ = (uint8_t)(noise_ >> 24); }
            am = pm = shValue_;
        }
        lastIndex_ = (uint8_t)idx;
        unsigned pmd = lfoPmd_ + extraPm_, amd = lfoAmd_ + extraAm_;
        if (pmd > 127) pmd = 127;
        if (amd > 127) amd = 127;
        if (lfoDelayLeft_ > 0)
        {
            const uint32_t total = lfoDelayTotal_ ? lfoDelayTotal_ : 1;
            const unsigned k = 256U - (lfoDelayLeft_ * 256U) / total;       // < 2^27: 32-bit math (no libgcc on arm32)
            pmd = (pmd * k) >> 8; amd = (amd * k) >> 8;
            lfoDelayLeft_ = lfoDelayLeft_ > (uint32_t)n ? lfoDelayLeft_ - (uint32_t)n : 0;
        }
        amRaw_ = (uint16_t)((am * amd) >> 7);
        pmRaw_ = (int16_t)(((int32_t)(int8_t)(uint8_t)pm * (int32_t)pmd) >> 7);
        for (int s = 0; s < kMaxVoices; ++s) if (v[s].active) updatePitch(s);
    }

    // ---- render ------------------------------------------------------------------------------
    // Adds n samples (n <= kMaxRender) of every active voice into out (mono). One carrier at full
    // level = 1.0. Voices that have finished are marked inactive.
    static constexpr int kMaxRender = 512;
    void render(float* out, int n)
    {
        if (n > kMaxRender) n = kMaxRender;
        for (int s = 0; s < n; ++s)
        {
            egAcc_ += egInc_;
            if (egAcc_ >= 65536U) { egAcc_ -= 65536U; ++egCounter_; if (!egCounter_) ++egCounter_; ticks_[s] = egCounter_; }
            else ticks_[s] = 0;
        }
        for (int slot = 0; slot < kMaxVoices; ++slot)
        {
            Voice& vc = v[slot];
            if (!vc.active) continue;
            switch (vc.alg & 7)
            {
                case 0: renderVoice<0>(vc, out, n); break;  case 1: renderVoice<1>(vc, out, n); break;
                case 2: renderVoice<2>(vc, out, n); break;  case 3: renderVoice<3>(vc, out, n); break;
                case 4: renderVoice<4>(vc, out, n); break;  case 5: renderVoice<5>(vc, out, n); break;
                case 6: renderVoice<6>(vc, out, n); break;  default: renderVoice<7>(vc, out, n); break;
            }
            if (!vc.keyed)
            {
                bool alive = false;     // ends when every operator is quiet after release
                for (auto& o : vc.o)
                    if (o.env <= kEgQuiet && (unsigned)(o.env >> o.shift) + o.tl < kEgQuiet) alive = true;
                if (!alive) { vc.active = false; vc.note = -1; }
            }
        }
    }

    // ---- display helpers -----------------------------------------------------------------------
    static double fixedHz(unsigned crs, unsigned fixrg, unsigned fine)
    {
        const unsigned base = (crs & 63) < 4 ? 8U + ((fine & 15) > 7 ? 7U : (fine & 15)) : 16U * ((crs & 63) >> 2) + (fine & 15);
        return (double)(base << (fixrg & 7));
    }

private:
    // Same arithmetic as the firmware's table-driven loop; the routing is resolved at compile time.
    template <int ALG>
    void renderVoice(Voice& vc, float* __restrict out, int n)
    {
        const Waves& W = waves();
        constexpr float kScale = 1.0f / 8192.0f;
        constexpr uint8_t i2 = kAlg[ALG][0], i3 = kAlg[ALG][1], i4 = kAlg[ALG][2];
        constexpr bool c1 = kAlg[ALG][3], c2 = kAlg[ALG][4], c3 = kAlg[ALG][5];
        const uint32_t am = vc.ams ? ((uint32_t)amRaw_ << (vc.ams - 1)) : 0;
        Op& o1 = vc.o[0]; Op& o2 = vc.o[1]; Op& o3 = vc.o[2]; Op& o4 = vc.o[3];
        const int fbl = vc.fbl;
        int32_t fb0 = vc.fb[0], fb1 = vc.fb[1];
        uint32_t p1 = o1.phase, p2 = o2.phase, p3 = o3.phase, p4 = o4.phase;
        const uint32_t n1 = o1.inc, n2 = o2.inc, n3 = o3.inc, n4 = o4.inc;
        auto pick = [](uint8_t sel, int32_t q1, int32_t q2, int32_t q3) -> int32_t {
            return sel == 1 ? q1 : sel == 2 ? q2 : sel == 3 ? q3 : sel == 5 ? q1 + q2 : sel == 6 ? q1 + q3 : sel == 7 ? q2 + q3 : 0; };
        // attenuation (env >> shift + AM + TL, clamped, << 2) only changes on EG ticks: cache it.
        // ~0u marks an operator below the quiet threshold (output 0), exactly as opOut() does.
        auto attq = [am](const Op& o) -> uint32_t {
            if (o.env > kEgQuiet) return ~0u;
            uint32_t att = (uint32_t)(o.env >> o.shift) + (o.ame ? am : 0) + o.tl;
            if (att > 0x3ff) att = 0x3ff;
            return att << 2;
        };
        uint32_t a1 = attq(o1), a2 = attq(o2), a3 = attq(o3), a4 = attq(o4);
        const uint16_t* w1 = W.w[o1.wave]; const uint16_t* w2 = W.w[o2.wave];
        const uint16_t* w3 = W.w[o3.wave]; const uint16_t* w4 = W.w[o4.wave];
        auto op = [](const uint16_t* w, uint32_t phase10, uint32_t aq) -> int32_t {
            if (aq == ~0u) return 0;
            const uint32_t sa = w[phase10 & 1023];
            const uint32_t total = (sa & 0x7fff) + aq;
            const int32_t vol = (int32_t)(opz_power_table[total & 0xff] >> (total >> 8));
            return (sa & 0x8000) ? -vol : vol;
        };
        for (int s = 0; s < n; ++s)
        {
            if (ticks_[s])
            {
                const uint32_t t = ticks_[s];
                clockEnv(o1, t); clockEnv(o2, t); clockEnv(o3, t); clockEnv(o4, t);
                a1 = attq(o1); a2 = attq(o2); a3 = attq(o3); a4 = attq(o4);
            }
            const int32_t mod = fbl ? (fb0 + fb1) >> (10 - fbl) : 0;
            const int32_t q1 = op(w1, (p1 >> 22) + (uint32_t)mod, a1);
            fb0 = fb1; fb1 = q1;
            const int32_t q2 = op(w2, (p2 >> 22) + (uint32_t)(pick(i2, q1, 0, 0) >> 1), a2);
            const int32_t q3 = op(w3, (p3 >> 22) + (uint32_t)(pick(i3, q1, q2, 0) >> 1), a3);
            int32_t r = op(w4, (p4 >> 22) + (uint32_t)(pick(i4, q1, q2, q3) >> 1), a4);
            if (c1) r += q1;
            if (c2) r += q2;
            if (c3) r += q3;
            p1 += n1; p2 += n2; p3 += n3; p4 += n4;
            out[s] += (float)r * kScale;
        }
        o1.phase = p1; o2.phase = p2; o3.phase = p3; o4.phase = p4;
        vc.fb[0] = fb0; vc.fb[1] = fb1;
    }

    static int32_t clampPitch(int p) { return p < 0 ? 0 : (p > 127 * 64 ? 127 * 64 : p); }
    static uint8_t effRate(unsigned raw, unsigned ksr) { if (!raw) return 0; raw += ksr; return (uint8_t)(raw > 63 ? 63 : raw); }

    // Pitch (1/64 semitone from MIDI 0) -> chip 10.10 phase step and keycode.
    static uint32_t pitchStep(int32_t pitch, unsigned* keycode)
    {
        int32_t k = pitch - 13 * 64;            // OPM block 0 starts at C#0 (MIDI 13)
        if (k < 0) k = 0;
        unsigned block = (unsigned)k / 768U, eff = (unsigned)k % 768U;
        if (block > 7) { block = 7; eff = 767; }
        const unsigned idx = eff >> 6, code = idx + idx / 3U;
        if (keycode) *keycode = (block << 2) | (code >> 2);
        return opz_phase_step[eff] >> (block ^ 7U);
    }

    void opPitch(Op& o, int32_t pitch)
    {
        if (o.fixed) { o.inc = o.fixedInc; return; }
        unsigned kc;
        uint32_t step = pitchStep(pitch + kDt2Delta[o.dt2 & 3], &kc);
        int32_t det = opz_detune1[kc][o.det1 & 3];
        if (o.det1 & 4) det = -det;
        int32_t s = (int32_t)step + det;
        if (s < 0) s = 0;
        step = ((uint32_t)s * o.multiple) >> 4;
        if (step > 400000U) step = 400000U;
        const double inc = step * stepScale_;
        o.inc = inc >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)inc;
    }

    void updatePitch(int slot)
    {
        Voice& vc = v[slot];
        int32_t delta = bend64_;
        if (vc.pms)
        {
            const int32_t pm = pmRaw_;
            delta += vc.pms < 6 ? (pm >> (6 - vc.pms)) : (pm * (1 << (vc.pms - 5)));
        }
        const int32_t pitch = vc.pitch64 + delta;
        for (auto& o : vc.o) opPitch(o, pitch);
    }

    static inline void clockEnv(Op& o, uint32_t counter)
    {
        if (o.state == EG_ATTACK && o.env == 0) o.state = EG_DECAY;
        if (o.state == EG_DECAY && o.env >= o.sustain) o.state = EG_SUSTAIN;
        if (o.state == EG_RELEASE && o.env >= 0xc0) o.state = EG_REVERB;
        const unsigned rate = o.rate[o.state];
        const unsigned shift = rate >> 2;
        const uint32_t c = counter << shift;
        if (c & 0x7ff) return;
        const unsigned bits = (c >> (shift <= 11 ? 11 : shift)) & 7;
        const unsigned inc = (opz_eg_increment[rate] >> (4 * bits)) & 15;
        int32_t env = o.env;
        if (o.state == EG_ATTACK)
        {
            if (rate < 62) env += (-(env + 1) * (int32_t)inc) >> 4;
            if (env < 0) env = 0;
        }
        else
        {
            env += (int32_t)inc;
            if (env > 0x3ff) env = 0x3ff;
        }
        o.env = (uint16_t)env;
    }

    static inline int32_t opOut(const Waves& W, const Op& o, uint32_t phase10, uint32_t am)
    {
        if (o.env > kEgQuiet) return 0;
        const uint32_t sa = W.w[o.wave][phase10 & 1023];
        uint32_t att = (uint32_t)(o.env >> o.shift) + (o.ame ? am : 0) + o.tl;
        if (att > 0x3ff) att = 0x3ff;
        const uint32_t total = (sa & 0x7fff) + (att << 2);
        const int32_t vol = (int32_t)(opz_power_table[total & 0xff] >> (total >> 8));
        return (sa & 0x8000) ? -vol : vol;
    }

    double sr_ = 44100.0, stepScale_ = 5196.0;
    uint32_t lfoX64Per128_ = 10390, fixedIncPerHz_ = 97392;
    uint32_t egInc_ = 27712, egAcc_ = 0, egCounter_ = 0;
    uint32_t ticks_[kMaxRender] = {};
    int bend64_ = 0;
    // LFO
    uint32_t lfoCounter_ = 0, lfoDelayTotal_ = 0, lfoDelayLeft_ = 0, noise_ = 0x1234567U;
    uint8_t lfoRate_ = 0x30, lfoWave_ = 2, lfoPmd_ = 0, lfoAmd_ = 0, shValue_ = 0, lastIndex_ = 0;
    bool lfoSync_ = false;
    unsigned extraPm_ = 0, extraAm_ = 0;
    int16_t pmRaw_ = 0;
    uint16_t amRaw_ = 0;
};

} // namespace opz
