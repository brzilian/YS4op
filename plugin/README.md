# YS4OP — 4-operator FM synth for MPC (OliveTreeDesign)

A 16-voice, 4-operator FM instrument modelled on the Yamaha YS200 (OPZ / TX81Z-family engine). It reads and receives YS200 voice data over sysex, ships with the YS200's 100 ROM voices as its presets, and exposes the YS200's "Easy Edit" controls on the MPC touch screen.

Requires MPC OS 3.10.1 or later, with **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support** turned on. Copy the folder `OliveTreeDesign - VST - YS4OP` into `VST2 Plugins` at the root of a USB drive, SD card or the internal drive. It then shows up under External Plugins as an instrument.

## Engine
Since 1.1 the voice engine is a chip-level YM2414 (OPZ) model, ported from the FM-4 firmware (github.com/brzilian/fm4, `firmware/app/fm4/opz.c`). It follows ymfm by Aaron Giles. At 44.1 kHz it renders all 100 ROM voices bit-identical to the firmware engine (checked sample by sample).
- 4 operators, the 8 Yamaha 4-op algorithms, OP4 feedback averaged over 2 samples, 8 waveforms (W1–W8) from the chip's log-sine / exp tables with 13-bit operator output
- Chip envelopes: increment tables, EG clock at chip rate / 3, exponential attack, D1L sustain threshold, rate scaling, EG shift (OP1 locked to 0, as on the TX81Z/TQ5) and the REV "reverb" release tail
- Pitch: OPM keycode phase steps in 1/64 semitone, TX81Z CRS → MUL/DT2 ratio map with FINE, DT1 detune table, fixed-frequency operators (8 Hz – 32 kHz)
- Chip LFO (saw, square, triangle, S&H) with the OPZ rate register, PMS/AMS shifts, delay fade-in and key sync. Mod wheel, breath (CC2), foot (CC4) and aftertouch add LFO depth through the voice's MW/BC/AT/FC settings
- Sample-rate independent: the chip's 55.93 kHz timing is rescaled, so it runs correctly at 44.1 or 48 kHz. Vibrato updates every 32 samples (the firmware: 128)
- Poly (16 voices, oldest-first stealing) or mono/legato, following the voice's POLY/MONO setting; pitch bend uses the voice's PB range; sustain (CC64), volume (CC7), program change 0–99
- Voice parameters are latched at key-on, as on the hardware: Easy Edit changes apply to the next notes you play
- The 10 YS200 effect presets, rebuilt with simple DSP (not sampled): Reverb Hall, Room and Plate, Delay, Delay L/R, Stereo Echo, Distortion + Reverb, Distortion + Echo, Gate Reverb, Reverse Gate

## Screen pages (YS200 Easy Edit)
| Page | Controls |
|---|---|
| EG | Attack, Decay, Release (±10 offsets, + = faster), Volume, EG Target (Vol+Tone / Volume / Tone), Voice |
| TONE | Brilliance (±10, modulator levels), Wave (±10, modulator coarse frequency), OP1–OP4 Wave (0–7, "Input-4Nos!") |
| LFO | LFO Speed, Vibrato, Tremolo (0–99; Vibrato and Tremolo set depth and sensitivity together, as on the YS200) |
| EFFECT | Effect preset, FX Time (0–40, shown in s or ms), FX Balance (0–99, 0 = off) |

The offsets are applied on top of the selected voice. The absolute controls (waves, LFO, effect) take the voice's own values whenever you select a voice or receive one over sysex. Turning them edits the voice. Edits and any sysex-loaded bank are saved with the MPC project.

## Presets and banks
The 100 programs are the YS200 ROM voices (00 Elegant … 99 Encore), compiled in from the supplied `ROM_YS.syx`. That file has four 32-voice dumps that overlap in steps of 25, which is how the YS200 lays out its 100 voices. `Content/Bank.syx` is read at load time. Replace it with another bank in the same format, or with a single 32-voice DX21/DX27/DX100/TX81Z dump (it lands on voices 00–31), to change the bank. The program list keeps the factory names, but the Voice control shows the loaded names.

## Sysex (received from the MPC track, any device number)
| Message | Effect |
|---|---|
| `F0 43 0n 03 00 5D <93 VCED> sum F7` | Single voice into the edit buffer |
| `F0 43 0n 04 20 00 <4096 VMEM> sum F7` | 32 voices into the bank, starting at the selected bank × 25 |
| `F0 43 0n 7E 00 21 "LM  8976AE" <23> sum F7` | ACED (fixed freq, fine, wave, EG shift, reverb rate, FC) |
| `F0 43 0n 7E 00 14 "LM  8023AE" <10> sum F7` | ACED2 (aftertouch) |
| `F0 43 0n 7E 00 0D "LM  8036EF" <3> sum F7` | EFEDS (effect preset, time, balance) |
| `F0 43 1n 12 pp vv F7` | VCED parameter change (0–92; 93 = operator on/off) |
| `F0 43 1n 13 pp vv F7` | ACED (0–22) / ACED2 (23–32) parameter change |
| `F0 43 1n 24 pp vv F7` | EFEDS parameter change (4–6); `pp = 07, vv = 1–4` selects the bank for VMEM dumps |

Dump requests are ignored, because the MPC discards MIDI that plugins send.

## Honest notes
- The chip model is exact where ymfm is (tables, envelopes, phase, LFO). The parts Yamaha never published follow the FM-4 firmware's documented defaults: the CPU-side curves for velocity, level scaling, LFO speed/depth and delay, and the effect algorithms (see the firmware's `docs/tq5_spec.md`).
- Not implemented, because the YS100/200 manual marks them as non-functional: portamento, DX21 pitch EG, chorus switch. EG-bias sensitivity (EBS), breath pitch bias and aftertouch pitch bias are also not applied, matching the FM-4 firmware.
- CPU: about 23–29 µs per 128-frame block with all 16 voices sounding plus reverb on a desktop CPU. By the SDK's rule of thumb (×20), that is roughly 16–20% of a block on the slowest MPCs (Cortex-A17) at absolute worst case. Cost scales with the number of sounding voices: about half that at the YS200's own 8-voice polyphony.
- The SDK's `framework/mpc_plugin.h` gained a small `sysexReceived()` hook, because the stock framework dropped sysex events.

## Build notes
Built with `tools/build.py --toolchain gcc`. Because zig couldn't be downloaded in this environment, the "gcc" cross compilers were stand-in wrappers around the system clang/lld. The wrappers used host libstdc++ headers (header-only), a patched glibc arch-header overlay, versioned link stubs and a tiny C++ runtime (`operator new/delete`, guards, static-destructor finalisation). Exceptions and RTTI are off. The validator passes for both binaries. Each needs only libc, libm and libdl, with glibc ≤ 2.29. The same code path, built for x86-64, passes the SDK host simulator and the project's test harness. A normal zig build (`python tools/build.py plugins/YS4OP`) works too.

## Licence
GPL-3.0-only, because the OPZ engine (`src/opz_engine.h`) is ported from the GPL-3.0 FM-4 firmware. The ymfm tables (`src/opz_ymfm_tables.h`) are BSD-3-Clause, Copyright (c) 2021 Aaron Giles (see `LICENSE-ymfm.txt`). `ys_patch.h` and `ys_effects.h` are MIT. The ROM voice data comes from the user's YS200 sysex file.
