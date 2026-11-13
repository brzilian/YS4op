#include "harness_inc.h"

int main(int argc, char** argv)
{
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); if (!h) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    entry = (Entry)dlsym(h, "VSTPluginMain");
    const char* syx = argv[2]; const char* outdir = argv[3];
    std::vector<uint8_t> bank = readFile(syx);

    // 1. every program: chord, check sound, finite, decays after release
    {
        Inst p; std::vector<float> allL, allR; int silent = 0, loud = 0, ringing = 0;
        for (int prog = 0; prog < 100; ++prog)
        {
            p.disp(effSetProgram, 0, prog);
            p.render(0.02);
            p.note(60, 100); p.note(64, 90); p.note(67, 80);
            std::vector<float> R; std::vector<float> L = p.render(1.2, &R);
            p.note(60, 0); p.note(64, 0); p.note(67, 0);
            std::vector<float> R2; std::vector<float> L2 = p.render(1.3, &R2);
            const double pk = peak(L), tail = peak(L2, L2.size() - 4410);
            if (pk < 0.01) { ++silent; std::printf("  silent program %d\n", prog); }
            if (pk > 0.999) { ++loud; std::printf("  clipping program %d (%.3f)\n", prog, pk); }
            if (tail > 0.05) ++ringing;
            if (prog < 100) { allL.insert(allL.end(), L.begin(), L.end()); allL.insert(allL.end(), L2.begin(), L2.end()); allR.insert(allR.end(), R.begin(), R.end()); allR.insert(allR.end(), R2.begin(), R2.end()); }
            char nm[64] = {0}; p.disp(effGetProgramName, 0, 0, nm);
            if (prog % 10 == 0 || prog == 99) std::printf("  %-14s peak %.3f  tail %.4f  voice='%s' fx='%s'\n", nm, pk, tail, p.display(1).c_str(), p.display(15).c_str());
            p.cc(123, 0); p.cc(120, 0); p.render(0.05);
        }
        CHECK(silent == 0, "all 100 programs make sound (%d silent)", silent);
        CHECK(loud == 0, "no program hits the clip ceiling (%d)", loud);
        std::printf("INFO  %d programs still ringing 1.2 s after release (long releases / reverb)\n", ringing);
        wav((std::string(outdir) + "/all_programs.wav").c_str(), allL, allR);
    }
    // 2. pitch: program with sine-ish carrier; FFT done in python. Render A4 on several voices.
    {
        Inst p; p.disp(effSetProgram, 0, 45);   // E.Organ 1
        p.note(69, 100); std::vector<float> R; std::vector<float> L = p.render(1.0, &R);
        wav((std::string(outdir) + "/a440_organ.wav").c_str(), L, R);
    }
    // 3. polyphony / CPU: 16 notes held + 8 more (stealing)
    {
        Inst p; p.disp(effSetProgram, 0, 0);
        double worst = 0, sum = 0; int blocks = 0; float L[128], R[128]; double pk = 0;
        for (int i = 0; i < 16; ++i) p.note(36 + i * 3, 100);
        for (int b = 0; b < 1000; ++b)
        {
            if (b == 300) for (int i = 0; i < 8; ++i) p.note(40 + i * 5, 110);
            p.block(L, R, 128); worst = std::fmax(worst, p.lastUs); sum += p.lastUs; ++blocks;
            for (int i = 0; i < 128; ++i) { if (!std::isfinite(L[i])) pk = 1e9; pk = std::fmax(pk, std::fabs(L[i])); }
        }
        std::printf("INFO  16-voice load: avg %.1f us, worst %.1f us per 128-frame block (2902 us budget) on this desktop\n", sum / blocks, worst);
        CHECK(pk < 1.0001 && pk > 0.05, "16-voice + stealing output finite and bounded (peak %.3f)", pk);
    }
    // 4. sysex: send bank dump; then a VMEM with bank select 1 whose slot 0 = voice 62 (Marimba)
    {
        Inst a; a.disp(effSetProgram, 0, 62); a.render(0.05); a.disp(effMainsChanged, 0, 0); a.disp(effMainsChanged, 0, 1); a.note(60, 100); std::vector<float> ref = a.render(0.8);
        Inst b; b.disp(effSetProgram, 0, 0); b.render(0.05);
        // build a VMEM: slot k = ROM voice 62+k (bank 3 VMEM holds voices 50..81 -> slot 12 = 62)
        // locate the 3rd VMEM in the file
        size_t pos = 0; int found = 0; std::vector<uint8_t> vmem;
        for (size_t i = 0; i + 4104 <= bank.size(); ++i) if (bank[i] == 0xF0 && bank[i + 3] == 0x04 && bank[i + 4103] == 0xF7) { if (++found == 3) { pos = i; break; } }
        std::vector<uint8_t> v(bank.begin() + pos, bank.begin() + pos + 4104);
        // move voice slot 12 (= 62) into slot 0, fix checksum
        std::memcpy(&v[6], &v[6 + 12 * 128], 128);
        int s = 0; for (int i = 0; i < 4096; ++i) s += v[6 + i]; v[6 + 4096] = (uint8_t)((-s) & 0x7F);
        b.sysex({0xF0, 0x43, 0x10, 0x24, 0x07, 0x01, 0xF7});
        b.sysex(v);
        b.render(0.05); b.disp(effMainsChanged, 0, 0); b.disp(effMainsChanged, 0, 1);
        b.note(60, 100); std::vector<float> got = b.render(0.8);
        double diff = 0; for (size_t i = 0; i < ref.size(); ++i) diff = std::fmax(diff, std::fabs(ref[i] - got[i]));
        CHECK(diff < 1e-4, "VMEM dump into bank 1 replaces the current voice (max diff vs program 62: %.2e)", diff);
        std::string want = a.display(1).substr(3); CHECK(b.display(1) == "00 " + want, "voice name follows the dump: '%s' (expected 00 %s)", b.display(1).c_str(), want.c_str());
        // parameter change: EFEDS effect preset -> 6 (Stereo Echo), balance 80
        b.sysex({0xF0, 0x43, 0x10, 0x24, 0x04, 0x06, 0xF7});
        b.sysex({0xF0, 0x43, 0x10, 0x24, 0x06, 80, 0xF7});
        b.render(0.01);
        CHECK(b.display(15) == "St Echo" && b.display(17) == "80", "EFEDS parameter change updates Effect='%s' Balance='%s'", b.display(15).c_str(), b.display(17).c_str());
        // VCED parameter change: LFS (54) = 77
        b.sysex({0xF0, 0x43, 0x10, 0x12, 54, 77, 0xF7}); b.render(0.01);
        CHECK(b.display(12) == "77", "VCED parameter change LFO speed -> '%s'", b.display(12).c_str());
        // VCED single voice dump built from current: algorithm 8 all carriers, name TESTVOICE
        std::vector<uint8_t> vced(93, 0);
        for (int op = 0; op < 4; ++op) { uint8_t* o = &vced[op * 13]; o[0] = 31; o[1] = 5; o[2] = 3; o[3] = 7; o[4] = 12; o[5] = 0; o[6] = 0; o[7] = 0; o[8] = 0; o[9] = 2; o[10] = 90; o[11] = 4 + op * 4; o[12] = 3; }
        vced[52] = 7; vced[54] = 35; vced[59] = 2; vced[62] = 24; vced[64] = 2; vced[71] = 50; vced[75] = 50;
        const char* nm = "TESTVOICE "; for (int i = 0; i < 10; ++i) vced[77 + i] = (uint8_t)nm[i];
        for (int i = 87; i < 93; ++i) vced[i] = i < 90 ? 99 : 50;
        std::vector<uint8_t> msg = {0xF0, 0x43, 0x00, 0x03, 0x00, 0x5D}; s = 0; for (uint8_t c : vced) { msg.push_back(c); s += c; } msg.push_back((uint8_t)((-s) & 0x7F)); msg.push_back(0xF7);
        b.sysex(msg); b.render(0.01);
        b.note(72, 100); std::vector<float> t = b.render(0.5);
        CHECK(peak(t) > 0.02 && peak(t) < 1.0, "VCED voice dump plays (peak %.3f)", peak(t));
        // state round trip after edits
        void* chunk = nullptr; VstIntPtr n = b.disp(effGetChunk, 0, 0, &chunk);
        std::vector<char> copy((char*)chunk, (char*)chunk + n);
        b.note(72, 0); b.cc(120, 0); b.render(0.1);
        Inst c; c.disp(effSetChunk, 0, (VstIntPtr)copy.size(), copy.data()); c.render(0.05);
        b.disp(effMainsChanged, 0, 0); b.disp(effMainsChanged, 0, 1); c.disp(effMainsChanged, 0, 0); c.disp(effMainsChanged, 0, 1);
        b.note(65, 100); c.note(65, 100);
        std::vector<float> x = b.render(0.5), y = c.render(0.5);
        diff = 0; for (size_t i = 0; i < x.size(); ++i) diff = std::fmax(diff, std::fabs(x[i] - y[i]));
        CHECK(diff < 1e-3, "state chunk (%d bytes) restores the sysex-edited voice (diff %.2e)", (int)n, diff);
        // full ROM bank file through sysex keeps everything in place
        Inst d; d.disp(effSetProgram, 0, 33); d.render(0.02);
        size_t i = 0; while (i < bank.size()) { if (bank[i] != 0xF0) { ++i; continue; } size_t j = i; while (bank[j] != 0xF7) ++j; d.sysex(std::vector<uint8_t>(bank.begin() + i, bank.begin() + j + 1)); i = j + 1; }
        d.render(0.02);
        CHECK(d.display(1) == "33 HuskyChoir", "full ROM bank via sysex keeps voice 33 = '%s'", d.display(1).c_str());
    }
    // 5. effects demo: same phrase through each effect
    {
        Inst p; p.disp(effSetProgram, 0, 79); std::vector<float> L, R;
        for (int fx = 0; fx <= 10; ++fx)
        {
            p.param(15, fx / 10.0f); p.param(17, 70 / 99.0f); p.param(16, 22 / 40.0f);
            p.render(0.02);
            for (int k = 0; k < 4; ++k) { p.note(60 + k * 4, 100); std::vector<float> r; auto l = p.render(0.2, &r); L.insert(L.end(), l.begin(), l.end()); R.insert(R.end(), r.begin(), r.end()); p.note(60 + k * 4, 0); }
            std::vector<float> r; auto l = p.render(1.4, &r); L.insert(L.end(), l.begin(), l.end()); R.insert(R.end(), r.begin(), r.end());
        }
        CHECK(peak(L) < 1.0 && peak(L) > 0.01, "effects pass finite (peak %.3f)", peak(L));
        wav((std::string(outdir) + "/effects.wav").c_str(), L, R);
    }
    std::printf("%s (%d failures)\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
