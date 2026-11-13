// YS4OP - a 4-operator FM synthesizer for the MPC, sysex compatible with the Yamaha YS200.
// By OliveTreeDesign.
//
// SPDX-License-Identifier: GPL-3.0-only   (the OPZ engine is ported from the GPL-3.0 FM-4 firmware)
//
// Signal flow
//   MIDI notes -> 16-voice allocator (or mono/legato when the voice says MONO)
//   -> per voice: 4 operators in one of 8 algorithms, YM2414 (OPZ) model (opz_engine.h, from FM-4)
//   -> mono sum -> YS200-style effect preset (ys_effects.h) -> stereo out -> master volume -> soft clip
//
// Sound data
//   The 100 YS200 ROM voices are compiled in (rombank.h, from the user's sysex file) and are also the
//   plugin's 100 programs. If Content/Bank.syx exists in the plugin folder it is parsed at load and
//   replaces the compiled-in bank (same format: optional "F0 43 1n 24 07 bb F7" bank selects followed by
//   32-voice VMEM dumps; bank bb lands on voice (bb-1)*25, like the hardware's overlapping banks).
//
// Sysex received from the MPC track (any device number n):
//   F0 43 0n 03 00 5D <93 VCED> sum F7                voice edit buffer (the current voice)
//   F0 43 0n 04 20 00 <4096 VMEM> sum F7              32 voices into the bank (at the selected bank)
//   F0 43 0n 7E 00 21 "LM  8976AE" <23 ACED> sum F7   additional voice parameters
//   F0 43 0n 7E 00 14 "LM  8023AE" <10 ACED2> sum F7  aftertouch parameters
//   F0 43 0n 7E 00 0D "LM  8036EF" <3 EFEDS> sum F7   effect preset / time / balance
//   F0 43 1n 12 pp vv F7    VCED parameter change (pp 0-92, 93 = operator on/off bits)
//   F0 43 1n 13 pp vv F7    ACED / ACED2 parameter change (0-22 / 23-32)
//   F0 43 1n 24 pp vv F7    EFEDS parameter change (pp 4-6); pp 7 = bank select for following VMEM dumps
// Dump requests cannot be answered: the MPC discards MIDI sent by plugins.
//
// The touch-screen parameters are the YS200's "Easy Edit" set:
//   EG     (target vol+tone / volume / tone, Attack, Decay, Release offsets -10..+10)
//   TONE   (Brilliance, Wave offsets -10..+10, Input-4Nos! = waveform 0-7 of each operator)
//   LFO    (Speed, Vibrato, Tremolo 0-99)
//   EFFECT (preset, Time 0-40, Balance 0-99)
// The offsets are applied on top of the selected voice; the absolute values follow the voice whenever a
// new voice is selected or received by sysex, and edit it when you turn them.

#include "mpc_plugin.h"
#include "opz_engine.h"
#include "ys_effects.h"
#include "ys_patch.h"
#include "rombank.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

enum ParamIndex
{
    kVolume = 0,      // master volume, dB (parameter 0: harmless when the host nudges it)
    kVoice,           // 0-99 voice number
    kEgTarget,        // Vol+Tone / Volume / Tone
    kAttack, kDecay, kRelease,
    kBrilliance, kWave,
    kWaveOp1, kWaveOp2, kWaveOp3, kWaveOp4,
    kLfoSpeed, kVibrato, kTremolo,
    kEffect, kFxTime, kFxBalance,
    kNumParams
};

constexpr int kFirstAbs = kWaveOp1;            // parameters that mirror the voice data
constexpr int kNumVoices = 16;
constexpr int kChunk = 256;
constexpr int kSysexSlots = 8;
constexpr int kSysexMax = 4200;

const char* const kEffectNames[11] = {
    "Off", "Rev Hall", "Rev Room", "Rev Plate", "Delay", "Delay L/R", "St Echo",
    "Dist+Rev", "Dist+Echo", "Gate Rev", "Rev Gate" };

// Easy-edit Vibrato / Tremolo <-> PMD/PMS, AMD/AMS (one knob drives depth and sensitivity together)
int vibratoFromPatch(const ys::Patch& p)
{
    const float d = p.pmd / 99.0f * opz::kPmsCents[p.pms & 7];
    return (int)std::lround(99.0f * std::sqrt(d / 700.0f));
}
void vibratoToPatch(int v, ys::Patch& p)
{
    if (v <= 0) { p.pmd = 0; return; }
    const float d = 700.0f * (v / 99.0f) * (v / 99.0f);
    int s = 1; while (s < 7 && opz::kPmsCents[s] < d) ++s;
    p.pms = (uint8_t)s;
    p.pmd = ys::lim((int)std::lround(99.0f * d / opz::kPmsCents[s]), 1, 99);
}
int tremoloFromPatch(const ys::Patch& p)
{
    const float d = p.amd / 99.0f * opz::kAmsDb[p.ams & 3];
    return (int)std::lround(99.0f * std::sqrt(d / 95.6f));
}
void tremoloToPatch(int v, ys::Patch& p)
{
    if (v <= 0) { p.amd = 0; return; }
    const float d = 95.6f * (v / 99.0f) * (v / 99.0f);
    int s = 1; while (s < 3 && opz::kAmsDb[s] < d) ++s;
    p.ams = (uint8_t)s;
    p.amd = ys::lim((int)std::lround(99.0f * d / opz::kAmsDb[s]), 1, 99);
    // tremolo only reaches operators with AM enabled; make sure the carriers have it
    bool any = false;
    for (int i = 0; i < 4; ++i) if (p.op[i].ame) any = true;
    if (!any) for (int i = 0; i < 4; ++i) if (ys::isCarrier(p.alg, i)) p.op[i].ame = 1;
}

inline float softClip(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

} // namespace

class YS4OP : public mpc::Plugin
{
public:
    YS4OP() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;

        addParam(floatParam("volume", "Volume", -40.0f, 6.0f, 0.0f, "dB", 1));
        addParam(intParam("voice", "Voice", 0, 99, 0));
        addParam(choiceParam("egtarget", "EG Target", {"Vol+Tone", "Volume", "Tone"}, 0));
        addParam(intParam("attack", "Attack", -10, 10, 0));
        addParam(intParam("decay", "Decay", -10, 10, 0));
        addParam(intParam("release", "Release", -10, 10, 0));
        addParam(intParam("brilliance", "Brilliance", -10, 10, 0));
        addParam(intParam("wave", "Wave", -10, 10, 0));
        addParam(intParam("wave_op1", "OP1 Wave", 0, 7, 0));
        addParam(intParam("wave_op2", "OP2 Wave", 0, 7, 0));
        addParam(intParam("wave_op3", "OP3 Wave", 0, 7, 0));
        addParam(intParam("wave_op4", "OP4 Wave", 0, 7, 0));
        addParam(intParam("lfo_speed", "LFO Speed", 0, 99, 35));
        addParam(intParam("vibrato", "Vibrato", 0, 99, 0));
        addParam(intParam("tremolo", "Tremolo", 0, 99, 0));
        addParam(choiceParam("effect", "Effect", std::vector<std::string>(kEffectNames, kEffectNames + 11), 0));
        addParam(intParam("fx_time", "FX Time", 0, 40, 20));
        addParam(intParam("fx_balance", "FX Balance", 0, 99, 0));

        // Voice bank: compiled-in ROM, optionally replaced by Content/Bank.syx
        std::memcpy(bank_, kRomBank, sizeof(bank_));
        {
            std::vector<uint8_t> file;
            if (mpc::readFile(mpc::contentPath("Bank.syx"), file) && !file.empty())
            {
                parseStream(file.data(), file.size());
                bankSelect_ = 0;
                bankModified_ = std::memcmp(bank_, kRomBank, sizeof(bank_)) != 0;
            }
        }
        // Programs = the 100 voices. Each sets the voice number and clears the Easy Edit offsets;
        // the absolute controls (waves, LFO, effect) then follow the voice automatically.
        for (int v = 0; v < 100; ++v)
        {
            ys::Patch p; ys::unpackVmem(bank_[v], p);
            char name[24]; std::snprintf(name, sizeof(name), "%02d %.10s", v, p.name);
            std::string n(name); while (!n.empty() && n.back() == ' ') n.pop_back();
            addProgram(n.c_str(), {0.0f, (float)v, 0, 0, 0, 0, 0, 0});
        }
        for (int v = 0; v < 100; ++v) { ys::Patch p; ys::unpackVmem(bank_[v], p); std::memcpy(names_[v], p.name, 10); }

        loadVoice(0);
        pushAbsParams(false);
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = (float)sampleRate;
        effects_.prepare(sampleRate);
        engine_.prepare(sampleRate);
        setupDirty_ = true;
    }

    void reset() override
    {
        if (stagePending_.exchange(false, std::memory_order_acq_rel))
        {
            std::memcpy(editVmem_, stagedVmem_, 128);
            ys::unpackVmem(editVmem_, edit_);
            opMask_ = stagedOpMask_;
            if (stagedHasBank_) { std::memcpy(bank_, stagedBank_, sizeof(bank_)); bankModified_ = true; refreshNames(); }
            currentVoice_ = stagedVoice_;
            for (int i = kFirstAbs; i < kNumParams; ++i) lastAbs_[i] = getInt(i);   // saved values are already in edit_
            setupDirty_ = true;
        }
        engine_.reset();
        for (int i = 0; i < kNumVoices; ++i) { held_[i] = false; sustained_[i] = false; order_[i] = 0; }
        monoStackSize_ = 0; sustain_ = false;
        ctrlCount_ = 0;
        wheel_ = breath_ = foot_ = after_ = 0; bend_ = 0;
        setupDirty_ = true;
        effects_.reset();
        gainSmooth_ = std::pow(10.0f, get(kVolume) / 20.0f);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        housekeeping();

        int frame = 0, ev = 0;
        while (frame < ctx.numFrames)
        {
            const int end = frame + kChunk < ctx.numFrames ? frame + kChunk : ctx.numFrames;
            int pos = frame;
            std::memset(mono_, 0, sizeof(float) * (size_t)(end - frame));
            while (pos < end)
            {
                while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= pos) handleMidi(ctx.midi[ev++]);
                int stop = end;
                if (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset < stop) stop = ctx.midi[ev].sampleOffset;
                if (stop <= pos) stop = pos + 1;
                renderVoices(mono_ + (pos - frame), stop - pos);
                pos = stop;
            }
            const int n = end - frame;
            effects_.process(mono_, wetL_, wetR_, n);
            const float target = std::pow(10.0f, get(kVolume) / 20.0f) * 0.28f;
            for (int i = 0; i < n; ++i)
            {
                gainSmooth_ += (target - gainSmooth_) * 0.25f;
                outL[frame + i] = softClip(wetL_[i] * gainSmooth_ * volumeCC_);
                if (outR) outR[frame + i] = softClip(wetR_[i] * gainSmooth_ * volumeCC_);
            }
            frame = end;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++]);
    }

    // Called by the framework for every sysex the MPC track sends. Copy only.
    void sysexReceived(const uint8_t* data, size_t size, int) override
    {
        if (size < 4 || size > (size_t)kSysexMax) return;
        const int w = sxWrite_.load(std::memory_order_relaxed);
        const int r = sxRead_.load(std::memory_order_acquire);
        if (((w + 1) % kSysexSlots) == r) return;              // full: drop
        std::memcpy(sxBuf_[w], data, size);
        sxLen_[w] = (int)size;
        sxWrite_.store((w + 1) % kSysexSlots, std::memory_order_release);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        const int v = (int)std::lround(plain);
        switch (index)
        {
            case kVoice:
            {
                const int k = v < 0 ? 0 : (v > 99 ? 99 : v);
                char nm[11]; std::memcpy(nm, names_[k], 10); nm[10] = 0;
                int e = 9; while (e >= 0 && nm[e] == ' ') nm[e--] = 0;
                std::snprintf(b, sizeof(b), "%02d %s", k, nm); out = b; return true;
            }
            case kAttack: case kDecay: case kRelease: case kBrilliance: case kWave:
                std::snprintf(b, sizeof(b), "%+03d", v); out = b; return true;
            case kFxTime:
            {
                const int fx = getInt(kEffect);
                const float t = v / 40.0f;
                if (fx == 4 || fx == 5 || fx == 6 || fx == 8)
                {
                    const float ms = 0.1f * std::pow(3000.0f, t);
                    if (ms < 10.0f) std::snprintf(b, sizeof(b), "%.1fms", ms); else std::snprintf(b, sizeof(b), "%.0fms", ms);
                }
                else if (fx == 9 || fx == 10) std::snprintf(b, sizeof(b), "%.1fs", 0.5f * std::pow(6.4f, t));
                else std::snprintf(b, sizeof(b), "%.1fs", 0.3f * std::pow(10.0f / 0.3f, t));
                out = b; return true;
            }
            default: return false;
        }
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        out.push_back('Y'); out.push_back('S'); out.push_back(1);
        out.push_back((uint8_t)currentVoice_);
        out.push_back(opMask_);
        out.push_back(bankModified_ ? 1 : 0);
        uint8_t v[128]; ys::packVmem(edit_, v);
        out.insert(out.end(), v, v + 128);
        if (bankModified_) out.insert(out.end(), &bank_[0][0], &bank_[0][0] + sizeof(bank_));
    }

    void loadExtraState(const uint8_t* d, size_t n) override
    {
        if (n < 6 + 128 || d[0] != 'Y' || d[1] != 'S' || d[2] != 1) return;
        stagedVoice_ = d[3] > 99 ? 99 : d[3];
        stagedOpMask_ = d[4] & 15;
        std::memcpy(stagedVmem_, d + 6, 128);
        stagedHasBank_ = d[5] == 1 && n >= 6 + 128 + sizeof(bank_);
        if (stagedHasBank_) std::memcpy(stagedBank_, d + 6 + 128, sizeof(bank_));
        stagePending_.store(true, std::memory_order_release);
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "YS4OP";
        info.vendor       = "OliveTreeDesign";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('O', 'i', 'T', '3');   // must match plugin.json "uid"
        info.version      = 1100;
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    // ---- voice data plumbing ---------------------------------------------------------------
    void loadVoice(int v)
    {
        currentVoice_ = v < 0 ? 0 : (v > 99 ? 99 : v);
        std::memcpy(editVmem_, bank_[currentVoice_], 128);
        ys::unpackVmem(editVmem_, edit_);
        opMask_ = 15;
        setupDirty_ = true;
    }

    void refreshNames() { for (int v = 0; v < 100; ++v) std::memcpy(names_[v], &bank_[v][57], 10); }

    // Write the voice's own values into the absolute parameters (Waves, LFO, Effect).
    void pushAbsParams(bool notify)
    {
        int vals[kNumParams] = {};
        for (int i = 0; i < 4; ++i) vals[kWaveOp1 + i] = edit_.op[i].osw;
        vals[kLfoSpeed] = edit_.lfs;
        vals[kVibrato] = vibratoFromPatch(edit_);
        vals[kTremolo] = tremoloFromPatch(edit_);
        vals[kEffect] = edit_.fxPreset;
        vals[kFxTime] = edit_.fxTime;
        vals[kFxBalance] = edit_.fxBalance;
        for (int i = kFirstAbs; i < kNumParams; ++i)
        {
            const bool changed = getInt(i) != vals[i];
            set(i, (float)vals[i]);
            lastAbs_[i] = vals[i];
            if (notify && changed) notifyHostParameterChanged(i);
        }
    }

    void housekeeping()
    {
        // 1. voice selection (program, Q-Link, MIDI program change)
        const int want = getInt(kVoice);
        if (want != currentVoice_) { loadVoice(want); pushAbsParams(true); }

        // 2. sysex
        bool editChanged = false;
        for (int guard = 0; guard < kSysexSlots; ++guard)
        {
            const int r = sxRead_.load(std::memory_order_relaxed);
            if (r == sxWrite_.load(std::memory_order_acquire)) break;
            if (handleSysex(sxBuf_[r], (size_t)sxLen_[r])) editChanged = true;
            sxRead_.store((r + 1) % kSysexSlots, std::memory_order_release);
        }
        if (editChanged) { setupDirty_ = true; pushAbsParams(true); }

        // 3. absolute controls turned by the user -> edit the voice
        for (int i = kFirstAbs; i < kNumParams; ++i)
        {
            const int v = getInt(i);
            if (v == lastAbs_[i]) continue;
            lastAbs_[i] = v;
            setupDirty_ = true;
            switch (i)
            {
                case kWaveOp1: case kWaveOp2: case kWaveOp3: case kWaveOp4: edit_.op[i - kWaveOp1].osw = (uint8_t)(v & 7); break;
                case kLfoSpeed: edit_.lfs = ys::lim(v, 0, 99); break;
                case kVibrato: vibratoToPatch(v, edit_); break;
                case kTremolo: tremoloToPatch(v, edit_); break;
                case kEffect: edit_.fxPreset = ys::lim(v, 0, 10); requestDisplayUpdate(); break;
                case kFxTime: edit_.fxTime = ys::lim(v, 0, 40); break;
                case kFxBalance: edit_.fxBalance = ys::lim(v, 0, 99); break;
                default: break;
            }
        }

        // 4. relative Easy Edit offsets
        const int offs[6] = { getInt(kEgTarget), getInt(kAttack), getInt(kDecay), getInt(kRelease), getInt(kBrilliance), getInt(kWave) };
        for (int i = 0; i < 6; ++i) if (offs[i] != lastOffs_[i]) { lastOffs_[i] = offs[i]; setupDirty_ = true; }

        if (setupDirty_)
        {
            setupDirty_ = false;
            ys::Patch p = edit_;
            const int target = offs[0];
            for (int i = 0; i < 4; ++i)
            {
                ys::OpParams& o = p.op[i];
                const bool car = ys::isCarrier(p.alg, i);
                const bool eg = target == 0 || (target == 1 && car) || (target == 2 && !car);
                if (eg)
                {
                    o.ar = ys::lim(o.ar + (int)std::lround(offs[1] * 1.6f), 0, 31);
                    o.d1r = ys::lim(o.d1r + (int)std::lround(offs[2] * 1.6f), 0, 31);
                    if (o.d2r > 0) o.d2r = ys::lim(o.d2r + (int)std::lround(offs[2] * 1.6f), 1, 31);
                    o.rr = ys::lim(o.rr + (int)std::lround(offs[3] * 0.75f), 1, 15);
                }
                if (!car)
                {
                    o.out = ys::lim(o.out + offs[4] * 2, 0, 99);     // Brilliance = modulator levels
                    o.crs = ys::lim(o.crs + offs[5], 0, 63);         // Wave = modulator coarse frequency
                }
            }
            renderPatch_ = p;                     // latched at key-on, as on the hardware
            engine_.lfoSetup(p);
            mono_on_ = p.mono != 0;
            bendRange_ = p.pbr;
            updateModDepth();
            engine_.setBend64(bend_ * bendRange_ * 64 / 8192);
            effects_.configure(p.fxPreset, p.fxTime, p.fxBalance);
        }
    }

    // ---- sysex ---------------------------------------------------------------------------------
    static bool checksumOk(const uint8_t* d, size_t n) { int s = 0; for (size_t i = 0; i < n; ++i) s += d[i]; return ((-s) & 0x7F) == d[n]; }

    // Parses a stream of messages (a .syx file). Used at load time only.
    void parseStream(const uint8_t* d, size_t n)
    {
        size_t i = 0;
        while (i < n)
        {
            if (d[i] != 0xF0) { ++i; continue; }
            size_t j = i + 1; while (j < n && d[j] != 0xF7) ++j;
            if (j >= n) break;
            handleSysex(d + i, j - i + 1);
            i = j + 1;
        }
        refreshNames();
    }

    // Returns true when the edit buffer (current voice) changed.
    bool handleSysex(const uint8_t* m, size_t n)
    {
        if (n < 6 || m[0] != 0xF0 || m[1] != 0x43 || m[n - 1] != 0xF7) return false;
        const int sub = m[2] & 0xF0;
        if (sub == 0x10 && n == 7)            // parameter change
        {
            const int group = m[3], pp = m[4], vv = m[5];
            if (group == 0x12)
            {
                if (pp == 93) { opMask_ = (uint8_t)(((vv >> 3) & 1) | (((vv >> 2) & 1) << 1) | (((vv >> 1) & 1) << 2) | ((vv & 1) << 3)); return true; }
                return ys::setVced(edit_, pp, vv);
            }
            if (group == 0x13) return ys::setAced(edit_, pp, vv);
            if (group == 0x24)
            {
                if (pp == 7) { bankSelect_ = vv >= 1 && vv <= 4 ? vv - 1 : 0; return false; }
                if (pp >= 4 && pp <= 6) return ys::setEfeds(edit_, pp - 4, vv);
            }
            return false;
        }
        if (sub != 0x00) return false;        // dump requests etc.
        const int fmt = m[3];
        if (fmt == 0x03 && n == 101 && checksumOk(m + 6, 93))
        {
            for (int i = 0; i < 93; ++i) ys::setVced(edit_, i, m[6 + i]);
            opMask_ = 15;
            return true;
        }
        if (fmt == 0x04 && n == 4104 && checksumOk(m + 6, 4096))
        {
            const int first = bankSelect_ * 25;
            bool hitCurrent = false;
            for (int k = 0; k < 32; ++k)
            {
                const int idx = first + k;
                if (idx >= 100) break;
                std::memcpy(bank_[idx], m + 6 + k * 128, 128);
                std::memcpy(names_[idx], m + 6 + k * 128 + 57, 10);
                if (idx == currentVoice_) hitCurrent = true;
            }
            bankModified_ = std::memcmp(bank_, kRomBank, sizeof(bank_)) != 0;
            if (hitCurrent) { std::memcpy(editVmem_, bank_[currentVoice_], 128); ys::unpackVmem(editVmem_, edit_); return true; }
            return false;
        }
        if (fmt == 0x7E && n > 6 + 10 + 1)
        {
            const int count = (m[4] << 7) | m[5];
            if ((size_t)count + 8 != n || !checksumOk(m + 6, (size_t)count)) return false;
            const uint8_t* id = m + 6; const uint8_t* data = m + 16;
            if (std::memcmp(id, "LM  8976AE", 10) == 0 && count == 33) { for (int i = 0; i < 23; ++i) ys::setAced(edit_, i, data[i]); return true; }
            if (std::memcmp(id, "LM  8023AE", 10) == 0 && count == 20) { for (int i = 0; i < 10; ++i) ys::setAced(edit_, 23 + i, data[i]); return true; }
            if (std::memcmp(id, "LM  8036EF", 10) == 0 && count == 13) { for (int i = 0; i < 3; ++i) ys::setEfeds(edit_, i, data[i]); return true; }
        }
        return false;
    }

    // ---- MIDI ------------------------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) noteOn(e.note(), e.velocity());
        else if (e.isNoteOff()) noteOff(e.note());
        else if (e.isPitchBend()) { bend_ = e.pitchBend(); engine_.setBend64(bend_ * bendRange_ * 64 / 8192); }
        else if (e.isAftertouch()) { after_ = e.data[1]; updateModDepth(); }
        else if (e.status() == 0xA0) { /* poly pressure: use the strongest as channel pressure */ const unsigned a = e.data[2]; if (a > after_ || a == 0) { after_ = a; updateModDepth(); } }
        else if (e.isProgramChange()) { const int p = e.data[1] > 99 ? 99 : e.data[1]; set(kVoice, (float)p); }
        else if (e.isController())
        {
            const int c = e.controller(), v = e.controllerValue();
            switch (c)
            {
                case 1: wheel_ = v; updateModDepth(); break;
                case 2: breath_ = v; updateModDepth(); break;
                case 4: foot_ = v; updateModDepth(); break;
                case 7: volumeCC_ = (v / 127.0f) * (v / 127.0f); break;
                case 64:
                {
                    const bool on = v >= 64;
                    if (sustain_ && !on)
                        for (int i = 0; i < kNumVoices; ++i) if (sustained_[i]) { sustained_[i] = false; engine_.keyOff(i); }
                    sustain_ = on; break;
                }
                case 120: for (int i = 0; i < kNumVoices; ++i) { engine_.kill(i); held_[i] = sustained_[i] = false; } monoStackSize_ = 0; break;   // all sound off
                case 121: wheel_ = breath_ = foot_ = after_ = 0; bend_ = 0; engine_.setBend64(0); updateModDepth(); break;
                case 123: allNotesOff(); break;
                default: break;
            }
        }
    }

    void allNotesOff()
    {
        for (int i = 0; i < kNumVoices; ++i) { held_[i] = false; sustained_[i] = false; engine_.keyOff(i); }
        monoStackSize_ = 0;
    }

    void noteOn(int note, int vel)
    {
        engine_.lfoKeyOn();
        if (mono_on_)
        {
            // last-note priority, legato while a key is held
            for (int i = 0; i < monoStackSize_; ++i) if (monoStack_[i] == note) { for (int k = i; k < monoStackSize_ - 1; ++k) monoStack_[k] = monoStack_[k + 1]; --monoStackSize_; break; }
            if (monoStackSize_ < 16) monoStack_[monoStackSize_++] = note;
            const bool legato = engine_.v[0].active && held_[0];
            engine_.keyOn(0, renderPatch_, opMask_, note, vel, legato);
            held_[0] = true; sustained_[0] = false;
            return;
        }
        int v = -1;
        for (int i = 0; i < kNumVoices; ++i) if (engine_.v[i].active && engine_.v[i].note == note) { v = i; break; }   // retrigger same key
        if (v < 0) for (int i = 0; i < kNumVoices; ++i) if (!engine_.v[i].active) { v = i; break; }
        if (v < 0)
        {
            // steal: oldest released voice first, then the oldest held one
            uint32_t best = 0xFFFFFFFFu;
            for (int pass = 0; pass < 2 && v < 0; ++pass)
                for (int i = 0; i < kNumVoices; ++i)
                {
                    const bool releasing = !held_[i] && !sustained_[i];
                    if ((pass == 0) != releasing) continue;
                    if (order_[i] < best) { best = order_[i]; v = i; }
                }
            if (v < 0) v = 0;
        }
        engine_.keyOn(v, renderPatch_, opMask_, note, vel, false);
        held_[v] = true; sustained_[v] = false; order_[v] = ++orderCounter_;
    }

    void noteOff(int note)
    {
        if (mono_on_)
        {
            for (int i = 0; i < monoStackSize_; ++i) if (monoStack_[i] == note) { for (int k = i; k < monoStackSize_ - 1; ++k) monoStack_[k] = monoStack_[k + 1]; --monoStackSize_; break; }
            if (engine_.v[0].note != note) return;
            if (monoStackSize_ > 0)
            {
                engine_.keyOn(0, renderPatch_, opMask_, monoStack_[monoStackSize_ - 1], engine_.v[0].vel, true);
                return;
            }
            held_[0] = false;
            if (sustain_) sustained_[0] = true; else engine_.keyOff(0);
            return;
        }
        for (int i = 0; i < kNumVoices; ++i)
            if (engine_.v[i].active && held_[i] && engine_.v[i].note == note)
            {
                held_[i] = false;
                if (sustain_) sustained_[i] = true; else engine_.keyOff(i);
            }
    }

    // ---- rendering ---------------------------------------------------------------------------------
    // LFO depth additions from the controllers, as the FM-4 firmware does (0..127 added to PMD/AMD).
    void updateModDepth()
    {
        const ys::Patch& f = renderPatch_;
        unsigned pm = (f.mwPitch * wheel_) / 99U + (f.bcPitch * breath_) / 99U + (f.atPitch * after_) / 99U + (f.fcPitch * foot_) / 99U;
        unsigned am = (f.mwAmp * wheel_) / 99U + (f.bcAmp * breath_) / 99U + (f.atAmp * after_) / 99U + (f.fcAmp * foot_) / 99U;
        engine_.lfoExtra(pm, am);
    }

    void renderVoices(float* out, int n)
    {
        int done = 0;
        while (done < n)
        {
            if (ctrlCount_ <= 0) { engine_.control(kCtrl); ctrlCount_ = kCtrl; }
            const int len = (n - done) < ctrlCount_ ? (n - done) : ctrlCount_;
            engine_.render(out + done, len);
            ctrlCount_ -= len;
            done += len;
        }
        for (int i = 0; i < kNumVoices; ++i) if (!engine_.v[i].active) { held_[i] = false; sustained_[i] = false; }
    }

    // state
    float sr_ = 44100.0f;
    uint8_t bank_[100][128];
    char names_[100][10];
    bool bankModified_ = false;
    int bankSelect_ = 0;
    int currentVoice_ = 0;
    ys::Patch edit_;
    uint8_t editVmem_[128] = {};
    uint8_t opMask_ = 15;
    int lastAbs_[kNumParams] = {};
    int lastOffs_[6] = {-99, -99, -99, -99, -99, -99};
    bool setupDirty_ = true;
    ys::Patch renderPatch_;
    fx::Processor effects_;

    opz::Engine engine_;
    bool held_[kNumVoices] = {}, sustained_[kNumVoices] = {};
    uint32_t order_[kNumVoices] = {}, orderCounter_ = 0;
    bool sustain_ = false, mono_on_ = false;
    int monoStack_[16] = {}, monoStackSize_ = 0;
    int bendRange_ = 2, bend_ = 0;
    unsigned wheel_ = 0, breath_ = 0, foot_ = 0, after_ = 0;
    static constexpr int kCtrl = 32;            // LFO / pitch update interval (samples)
    int ctrlCount_ = 0;
    float volumeCC_ = 1.0f, gainSmooth_ = 0.28f;

    float mono_[kChunk], wetL_[kChunk], wetR_[kChunk];

    // sysex FIFO (sysexReceived -> process)
    uint8_t sxBuf_[kSysexSlots][kSysexMax];
    int sxLen_[kSysexSlots] = {};
    std::atomic<int> sxWrite_{0}, sxRead_{0};

    // state staging (loadExtraState -> reset on the audio thread)
    std::atomic<bool> stagePending_{false};
    uint8_t stagedVmem_[128] = {};
    uint8_t stagedBank_[100][128];
    bool stagedHasBank_ = false;
    int stagedVoice_ = 0;
    uint8_t stagedOpMask_ = 15;
};

MPC_PLUGIN_ENTRY(YS4OP)
