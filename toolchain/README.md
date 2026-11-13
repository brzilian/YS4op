# Clang cross-toolchain (optional)

The SDK normally builds with zig, which `tools/build.py` installs from PyPI. If you can't install zig, these scripts build the same MPC binaries with an installed clang + lld (LLVM 18 was used). They pose as the `--toolchain gcc` cross-compilers (`arm-linux-gnueabihf-g++`, `aarch64-linux-gnu-g++`).

```sh
python3 toolchain/make_overlay.py          # once: headers overlay, stub libraries, compiler wrappers
PATH=$PWD/toolchain/bin:$PATH python3 <sdk>/tools/build.py <sdk>/plugins/YS4OP --toolchain gcc
```

How it works:
- **Headers:** the host's glibc and libstdc++ 13 headers are used for the ARM targets, through a small patched overlay (word size, no `_Float128`, float evaluation, pre-2.38 scanf symbols, no extern templates).
- **Linking:** against stub `libc.so.6` / `libm.so.6` / `libdl.so.2` that carry only symbol names and glibc versions (GLIBC_2.4 on arm32, 2.17 on arm64, plus 2.27 for `expf`/`logf`/`powf`). At run time the MPC's real libraries are used.
- **C++ runtime:** no libstdc++ or libsupc++ is linked. `rt/rt.cpp` supplies `operator new/delete`, static-init guards, `__dso_handle`, the `std::__throw_*` hooks (they abort), and runs static destructors on `dlclose`. Exceptions and RTTI are off.
- **Code generation:** arm32 `-mcpu=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard`, arm64 `-march=armv8-a -mtune=cortex-a55`.

The SDK's validator passes for both binaries; each imports only libc, libm and libdl, with glibc ≤ 2.27. Built from a fresh SDK checkout (commit febd2ca) with `sdk-patch/` applied, the result is byte-identical to the binaries in `dist/`.
