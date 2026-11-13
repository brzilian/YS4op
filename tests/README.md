# Tests

Native (desktop) tests. Build the plugin first (`tools/build.py ... --target native`), which gives `plugins/YS4OP/build/native/ys4op.so`.

| Test | What it checks | Build |
|---|---|---|
| `harness.cpp` | All 100 programs sound and don't clip; 16-voice + stealing; VMEM bank dump with bank select; EFEDS/VCED parameter changes; VCED voice dump; state round-trip; full ROM bank over sysex; every effect | `g++ -O2 -std=c++17 -I <sdk>/framework harness.cpp -o harness -ldl` then `./harness ys4op.so ROM_YS.syx outdir` |
| `bench.cpp` | CPU per 128-frame block for 1 and 16 voices, effect on/off | same flags; `./bench ys4op.so` |
| `ab.cpp` | A/B WAV of two plugin builds (e.g. 1.0 vs 1.1 engine) | same flags; `./ab old.so new.so out.wav` |
| `cmp.cpp` | **Bit-exactness**: renders all 100 ROM voices through FM-4's `firmware/app/fm4/opz.c` and through `plugin/src/opz_engine.h`, then compares sample by sample (expect `0.000e+00`) | `gcc -O2 -std=c99 -DFM4_HOST -I <fm4>/firmware/app/fm4 -c <fm4>/firmware/app/fm4/opz.c <fm4>/firmware/app/fm4/opz_ymfm_tables.c` then `g++ -O2 -std=c++17 -DFM4_HOST -I <fm4>/firmware/app/fm4 -I ../plugin/src cmp.cpp opz.o opz_ymfm_tables.o -o cmp` |
| `pitch.cpp` | Tuning of ratio and fixed-frequency operators at 44.1 and 48 kHz | `g++ -O2 -std=c++17 -I ../plugin/src pitch.cpp -o pitch` |
| `mkbank.py` | Regenerates `plugin/src/rombank.h` from a YS200 bank `.syx` | `python3 mkbank.py ROM_YS.syx ../plugin/src/rombank.h` |

The SDK's own host simulator: `<sdk>/.sdk-tools/host_sim ys4op.so test` (19 checks, all pass; also with `--sr 48000`).
