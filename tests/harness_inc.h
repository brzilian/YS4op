// Shared test-host code (split from harness.cpp so bench.cpp and ab.cpp can reuse it).
// Native test harness for YS4OP: programs, polyphony/CPU, sysex (VMEM/VCED/param change), state.
#include "mpc_vst2_abi.h"
#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{ return op == 1 /*audioMasterVersion*/ ? 2400 : 0; }

typedef AEffect* (*Entry)(audioMasterCallback);
static Entry entry;
static int fails = 0;
#define CHECK(c, ...) do { if (c) { std::printf("PASS  "); } else { std::printf("FAIL  "); ++fails; } std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

struct Inst
{
    AEffect* e;
    Inst() { e = entry(host); disp(effSetSampleRate, 0, 0, nullptr, 44100.0f); disp(effSetBlockSize, 0, 128); disp(effOpen); disp(effMainsChanged, 0, 1); }
    ~Inst() { disp(effMainsChanged, 0, 0); disp(effClose); }
    VstIntPtr disp(int op, int idx = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e->dispatcher(e, op, idx, v, p, o); }
    std::vector<VstMidiEvent> midi; std::vector<std::vector<char>> sx; std::vector<VstMidiSysexEvent> sxe;
    void note(int n, int v) { VstMidiEvent m{}; m.type = kVstMidiType; m.byteSize = sizeof(m); m.midiData[0] = (char)(v ? 0x90 : 0x80); m.midiData[1] = (char)n; m.midiData[2] = (char)v; midi.push_back(m); }
    void cc(int c, int v) { VstMidiEvent m{}; m.type = kVstMidiType; m.byteSize = sizeof(m); m.midiData[0] = (char)0xB0; m.midiData[1] = (char)c; m.midiData[2] = (char)v; midi.push_back(m); }
    void sysex(const std::vector<uint8_t>& d) { sx.emplace_back(d.begin(), d.end()); }
    double lastUs = 0;
    void block(float* L, float* R, int n)
    {
        std::vector<VstEvent*> ptrs;
        sxe.resize(sx.size());
        for (size_t i = 0; i < sx.size(); ++i) { VstMidiSysexEvent& s = sxe[i]; std::memset(&s, 0, sizeof(s)); s.type = kVstSysExType; s.byteSize = sizeof(s); s.dumpBytes = (int)sx[i].size(); s.sysexDump = sx[i].data(); ptrs.push_back((VstEvent*)&s); }
        for (auto& m : midi) ptrs.push_back((VstEvent*)&m);
        std::vector<char> buf(sizeof(VstEvents) + ptrs.size() * sizeof(void*));
        VstEvents* ev = (VstEvents*)buf.data(); ev->numEvents = (int)ptrs.size();
        for (size_t i = 0; i < ptrs.size(); ++i) ev->events[i] = ptrs[i];
        disp(effProcessEvents, 0, 0, ev);
        float* outs[2] = {L, R};
        auto t0 = std::chrono::steady_clock::now();
        e->processReplacing(e, nullptr, outs, n);
        lastUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        midi.clear(); sx.clear();
    }
    std::vector<float> render(double secs, std::vector<float>* right = nullptr)
    {
        std::vector<float> out; int total = (int)(secs * 44100);
        float L[128], R[128];
        for (int i = 0; i < total; i += 128) { block(L, R, 128); out.insert(out.end(), L, L + 128); if (right) right->insert(right->end(), R, R + 128); }
        return out;
    }
    void param(int i, float norm) { e->setParameter(e, i, norm); }
    std::string display(int i) { char b[64] = {0}; disp(effGetParamDisplay, i, 0, b); return b; }
};

static void wav(const char* path, const std::vector<float>& L, const std::vector<float>& R)
{
    FILE* f = std::fopen(path, "wb"); if (!f) return;
    uint32_t n = (uint32_t)L.size(), bytes = n * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i) { int16_t a = (int16_t)std::lrint(std::fmax(-1, std::fmin(1, L[i])) * 32767), b = (int16_t)std::lrint(std::fmax(-1, std::fmin(1, R[i])) * 32767); std::fwrite(&a, 2, 1, f); std::fwrite(&b, 2, 1, f); }
    std::fclose(f);
}

static double peak(const std::vector<float>& v, size_t a = 0, size_t b = (size_t)-1) { double p = 0; for (size_t i = a; i < v.size() && i < b; ++i) { if (!std::isfinite(v[i])) return 1e9; p = std::fmax(p, std::fabs(v[i])); } return p; }

static std::vector<uint8_t> readFile(const char* p) { std::vector<uint8_t> d; FILE* f = std::fopen(p, "rb"); if (!f) return d; int c; while ((c = std::fgetc(f)) != EOF) d.push_back((uint8_t)c); std::fclose(f); return d; }

