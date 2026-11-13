// ys_effects.h - the YS200's built-in effect presets, re-created (not sampled) with simple DSP.
//
// SPDX-License-Identifier: MIT
//
// Preset numbers follow the EFEDS "effect preset" byte: 0 = no effect, 1..10 = the manual's No.0..No.9
//   1 Reverb Hall   2 Reverb Room   3 Reverb Plate   4 Delay   5 Delay L/R   6 Stereo Echo
//   7 Distortion + Reverb   8 Distortion + Echo   9 Gate Reverb   10 Reverse Gate
// Time 0..40 maps (exponentially) onto the ranges printed in the manual:
//   reverbs 0.3 - 10.0 s (RT60), delays 0.1 - 300 ms, gate "room size" 0.5 - 3.2 s.
// Balance 0..99 is the effect level against the voice; 0 turns the effect off.
//
// Reverb: 4-line feedback delay network (Householder matrix), damping in the loop, two input
// allpass diffusers, stereo taps. Gate/reverse: 24-tap diffuse burst on a delay line with a flat or
// rising envelope. All memory is allocated in prepare().

#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fx {

struct Line
{
    std::vector<float> buf; int mask = 0, pos = 0;
    void prepare(int minLen) { int n = 1; while (n < minLen) n <<= 1; buf.assign((size_t)n, 0.0f); mask = n - 1; pos = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    inline void write(float x) { buf[(size_t)pos] = x; pos = (pos + 1) & mask; }
    inline float tap(int d) const { return buf[(size_t)((pos - d) & mask)]; }   // d >= 1
    inline float tapFrac(float d) const
    {
        const int i = (int)d; const float f = d - (float)i;
        return tap(i) + (tap(i + 1) - tap(i)) * f;
    }
};

struct Allpass
{
    Line l; int d = 1; float g = 0.6f;
    inline float process(float x) { const float y = l.tap(d); const float v = x + g * y; l.write(v); return y - g * v; }
};

inline float softDist(float x) { const float a = x * 6.0f; return (a / (1.0f + std::fabs(a))) * 0.5f; }

class Processor
{
public:
    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        static const int base[4] = {1687, 1999, 2371, 2777};      // primes around 38..63 ms at 44.1 kHz
        for (int i = 0; i < 4; ++i) fdn_[i].prepare((int)(base[i] * 1.6f * sr_ / 44100.0f) + 8);
        const int apLen[4] = {142, 107, 379, 277};
        for (int i = 0; i < 4; ++i) { ap_[i].l.prepare(apLen[i] + 2); ap_[i].d = apLen[i]; }
        pre_.prepare((int)(0.05f * sr_) + 4);
        delayL_.prepare((int)(0.70f * sr_) + 4);
        delayR_.prepare((int)(0.70f * sr_) + 4);
        burst_.prepare((int)(1.0f * sr_) + 4);
        for (int i = 0; i < kTaps; ++i)    // fixed pseudo-random tap pattern
        {
            uint32_t h = 2654435761u * (uint32_t)(i + 1);
            tapJitter_[i] = ((h >> 8) & 0xFFFF) / 65535.0f;
            tapSign_[i] = (h & 1) ? 1.0f : -1.0f;
        }
        configured_ = -1;
        reset();
    }

    void reset()
    {
        for (auto& l : fdn_) l.clear();
        for (auto& a : ap_) a.l.clear();
        pre_.clear(); delayL_.clear(); delayR_.clear(); burst_.clear();
        for (float& d : damp_) d = 0.0f;
        follower_ = 0.0f; gate_ = 0.0f; hold_ = 0;
        wet_ = 0.0f;
    }

    // preset 0..10, time 0..40, balance 0..99
    void configure(int preset, int time, int balance)
    {
        const int key = preset * 10000 + time * 100 + balance;
        if (key == configured_) return;
        configured_ = key;
        preset_ = preset;
        const float t = time / 40.0f;
        targetWet_ = (preset == 0) ? 0.0f : balance / 99.0f;
        // reverbs
        float rt = 0.3f * std::pow(10.0f / 0.3f, t);
        float size = 1.0f, damp = 0.25f, predelay = 0.012f;
        switch (preset)
        {
            case 1: size = 1.0f;  damp = 0.30f; predelay = 0.025f; break;     // hall
            case 2: size = 0.55f; damp = 0.45f; predelay = 0.006f; break;     // room
            case 3: size = 0.75f; damp = 0.10f; predelay = 0.001f; break;     // plate
            case 7: size = 0.9f;  damp = 0.35f; predelay = 0.015f; break;     // dist + reverb
            default: break;
        }
        static const int base[4] = {1687, 1999, 2371, 2777};
        for (int i = 0; i < 4; ++i)
        {
            len_[i] = (int)(base[i] * size * 1.6f * sr_ / 44100.0f);
            if (len_[i] < 64) len_[i] = 64;
            if (len_[i] > fdn_[i].mask - 2) len_[i] = fdn_[i].mask - 2;
            fb_[i] = std::pow(10.0f, -3.0f * len_[i] / (rt * sr_));
        }
        dampCoef_ = damp;
        preDelay_ = (int)(predelay * sr_) + 1;
        // delays
        const float ms = 0.1f * std::pow(3000.0f, t);                     // 0.1 .. 300 ms
        delaySamples_ = ms * 0.001f * sr_; if (delaySamples_ < 2.0f) delaySamples_ = 2.0f;
        echoFb_ = 0.55f;
        // gates: room size 0.5 .. 3.2 s -> burst length (the audible gate) of size / 4
        const float room = 0.5f * std::pow(3.2f / 0.5f, t);
        burstLen_ = room * 0.25f * sr_;
        if (burstLen_ > burst_.mask - 4) burstLen_ = (float)(burst_.mask - 4);
        for (int i = 0; i < kTaps; ++i)
        {
            const float pos = (i + tapJitter_[i]) / kTaps;                  // 0..1 along the burst
            tapPos_[i] = 1 + (int)(pos * burstLen_);
            const float env = preset == 10 ? (0.1f + pos * pos * 1.6f) : (1.0f - 0.25f * pos);
            tapGain_[i] = tapSign_[i] * env * (1.6f / kTaps * 4.0f);
        }
    }

    bool active() const { return preset_ != 0 && (targetWet_ > 0.0f || wet_ > 0.0001f); }

    // in: mono voice sum; out: stereo (overwritten)
    void process(const float* in, float* outL, float* outR, int n)
    {
        if (!active())
        {
            for (int i = 0; i < n; ++i) { outL[i] = in[i]; outR[i] = in[i]; }
            return;
        }
        const float wetStep = (targetWet_ - wet_) / (float)(n > 0 ? n : 1);
        const bool dist = preset_ == 7 || preset_ == 8;
        for (int i = 0; i < n; ++i)
        {
            wet_ += wetStep;
            float x = in[i];
            float dry = x;
            float wl = 0.0f, wr = 0.0f;
            if (dist) { x = softDist(x); }
            switch (preset_)
            {
                case 1: case 2: case 3: case 7: reverb(x, wl, wr); if (dist) { wl += x; wr += x; } break;
                case 4: { const float y = delayL_.tapFrac(delaySamples_); delayL_.write(x); wl = wr = y; break; }
                case 5:
                {
                    const float yl = delayL_.tapFrac(delaySamples_);
                    const float yr = delayL_.tapFrac(delaySamples_ * 2.0f > delayL_.mask - 2 ? delayL_.mask - 2 : delaySamples_ * 2.0f);
                    delayL_.write(x); wl = yl; wr = yr; break;
                }
                case 6: case 8:
                {
                    const float yl = delayL_.tapFrac(delaySamples_);
                    const float yr = delayR_.tapFrac(delaySamples_ * 1.03f);
                    delayL_.write(x + yl * echoFb_); delayR_.write(x + yr * echoFb_);
                    wl = yl; wr = yr;
                    if (dist) { wl += x; wr += x; }
                    break;
                }
                case 9: case 10:
                {
                    // gate: the burst only follows the input while it is loud enough (gate reverb);
                    // reverse gate swells up across the burst then stops dead.
                    const float a = std::fabs(x);
                    follower_ += (a - follower_) * (a > follower_ ? 0.01f : 0.0005f);
                    burst_.write(diffuse(x));
                    float sl = 0.0f, sr = 0.0f;
                    for (int k = 0; k < kTaps; ++k)
                    {
                        const float v = burst_.tap(tapPos_[k]) * tapGain_[k];
                        if (k & 1) sr += v; else sl += v;
                    }
                    if (preset_ == 9)
                    {
                        if (follower_ > 0.01f) { hold_ = (int)burstLen_; }
                        if (hold_ > 0) { --hold_; gate_ += (1.0f - gate_) * 0.01f; }
                        else gate_ *= 0.995f;
                        sl *= gate_; sr *= gate_;
                    }
                    wl = sl; wr = sr;
                    break;
                }
                default: break;
            }
            const float dryGain = dist ? (1.0f - wet_) : (1.0f - 0.35f * wet_);
            outL[i] = dry * dryGain + wl * wet_;
            outR[i] = dry * dryGain + wr * wet_;
        }
    }

private:
    static constexpr int kTaps = 24;

    inline float diffuse(float x) { return ap_[1].process(ap_[0].process(x)); }

    inline void reverb(float x, float& l, float& r)
    {
        const float p = pre_.tap(preDelay_); pre_.write(x);
        float d = diffuse(p);
        if (preset_ == 3) d = ap_[3].process(ap_[2].process(d));          // plate: extra density
        float y[4];
        for (int i = 0; i < 4; ++i)
        {
            float v = fdn_[i].tap(len_[i]);
            damp_[i] += (v - damp_[i]) * (1.0f - dampCoef_);              // one-pole lowpass in the loop
            y[i] = damp_[i] * fb_[i];
        }
        const float s = (y[0] + y[1] + y[2] + y[3]) * 0.5f;                 // Householder reflection
        for (int i = 0; i < 4; ++i) fdn_[i].write(d + (y[i] - s));
        l = (y[0] + y[2]) * 0.7f;
        r = (y[1] + y[3]) * 0.7f;
    }

    float sr_ = 44100.0f;
    int configured_ = -1, preset_ = 0;
    float targetWet_ = 0.0f, wet_ = 0.0f;
    Line fdn_[4]; int len_[4] = {1000, 1100, 1200, 1300}; float fb_[4] = {0, 0, 0, 0}, damp_[4] = {0, 0, 0, 0};
    float dampCoef_ = 0.3f;
    Allpass ap_[4];
    Line pre_; int preDelay_ = 1;
    Line delayL_, delayR_; float delaySamples_ = 100.0f, echoFb_ = 0.5f;
    Line burst_; float burstLen_ = 1000.0f;
    int tapPos_[kTaps] = {}; float tapGain_[kTaps] = {}, tapJitter_[kTaps] = {}, tapSign_[kTaps] = {};
    float follower_ = 0.0f, gate_ = 0.0f; int hold_ = 0;
};

} // namespace fx
