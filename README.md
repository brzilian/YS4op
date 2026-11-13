# YS4op

**YS4OP** by OliveTreeDesign: a 16-voice, 4-operator FM instrument plugin for Akai MPC standalone hardware, sysex compatible with the Yamaha YS200.

- **Engine:** a chip-level YM2414 (OPZ) model, ported from the [FM-4 firmware](https://github.com/brzilian/fm4) and bit-exact with it at 44.1 kHz. It follows [ymfm](https://github.com/aaronsgiles/ymfm).
- **Presets:** the YS200's 100 ROM voices.
- **Screen:** the YS200 "Easy Edit" controls on four pages (EG, TONE, LFO, EFFECT).
- **Sysex:** VMEM bank dumps (including YS200 bank select), VCED / ACED / ACED2 / EFEDS dumps and parameter changes.
- **Effects:** the 10 YS200 effect presets.

Built with the [inMusic MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). The full plugin documentation is in [`plugin/README.md`](plugin/README.md).

## Install on the MPC

1. Unzip [`dist/YS4OP-OliveTreeDesign-1.1.0-mpc.zip`](dist/YS4OP-OliveTreeDesign-1.1.0-mpc.zip).
2. Copy the folder `OliveTreeDesign - VST - YS4OP` into `VST2 Plugins` at the root of a USB drive, SD card or the internal drive.
3. The MPC must run **MPC OS 3.10.1 or later**, with **Preferences → Enhancements (Beta) → Enable Enhancements** and **External Plugin Support** turned on.
4. Load YS4OP from External Plugins as an instrument.

To use a different voice bank, replace `Content/Bank.syx` in the plugin folder. It accepts the YS200's 4 × VMEM-with-bank-select layout, or a single 32-voice DX21/DX27/DX100/TX81Z bank.

## Repository layout

| Path | Contents |
|---|---|
| `plugin/` | The SDK project: `plugin.json`, `ui.json`, `src/`, `Content/Bank.syx`. Copy it to `<sdk>/plugins/YS4OP` |
| `sdk-patch/` | The `sysexReceived()` hook added to the SDK's `framework/mpc_plugin.h` (the stock framework drops sysex) |
| `tests/` | Native test harness, CPU benchmark, A/B renderer, bit-exactness test against FM-4's engine, tuning test |
| `toolchain/` | Optional clang cross-toolchain for building without zig (reproduces `dist/` byte for byte) |
| `dist/` | Ready-to-install MPC package |

## Build

```sh
git clone https://github.com/inmusic-software/mpc-plugin-builder-sdk.git sdk
cd sdk && git apply ../YS4op/sdk-patch/mpc_plugin_sysex.patch
mkdir -p plugins/YS4OP && cp -r ../YS4op/plugin/. plugins/YS4OP/
python3 tools/build.py plugins/YS4OP                 # zig toolchain (installed by the SDK)
.sdk-tools/host_sim plugins/YS4OP/build/native/ys4op.so test
python3 tools/package.py plugins/YS4OP
```

To build without zig, see [`toolchain/README.md`](toolchain/README.md).

## Status

- **Simulator:** passes the SDK validator and the host simulator (19/19 checks, at 44.1 and 48 kHz), plus the project tests in `tests/`.
- **Hardware:** not yet tested on an MPC.
- **CPU:** about 23–29 µs per 128-frame block with all 16 voices plus reverb on a desktop. That is roughly 16–20% of a block on the slowest MPCs at the absolute worst case, and about half that at 8 voices.

## Licence

GPL-3.0-only (see `LICENSE`), because the OPZ engine is ported from the GPL-3.0 FM-4 firmware. The ymfm tables are BSD-3-Clause, Copyright (c) 2021 Aaron Giles (`plugin/LICENSE-ymfm.txt`). `plugin/src/ys_patch.h` and `plugin/src/ys_effects.h` are MIT. The ROM voice data in `plugin/Content/Bank.syx` and `plugin/src/rombank.h` is the Yamaha YS200 factory bank. "Yamaha", "YS200", "TX81Z" and "DX" are Yamaha trademarks, used only to describe compatibility; this project is not affiliated with Yamaha or inMusic.
