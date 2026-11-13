#!/usr/bin/env python3
"""Prepare the clang cross-toolchain used to build YS4OP without zig (see toolchain/README.md).

Creates, next to this script:
  archinc/   glibc arch headers copied from the host (x86-64) and patched for ARM (32/64-bit)
  cxxarch/   libstdc++ arch config copied from the host, extern templates disabled
  stubs/     link-time stub libc/libm/libdl/libgcc_s with glibc symbol versions (mkstubs.py)
  bin/       arm-linux-gnueabihf-g++ and aarch64-linux-gnu-g++ -> xcxx.py

Needs: clang + lld (LLVM 18 was used), host glibc and libstdc++ 13 headers (Ubuntu 24.04 layout).
Only headers we actually include are relied on; ABI-relevant libc use is limited to opaque FILE*,
Dl_info, malloc/free and string/math functions (verified via the imported-symbol list).
"""
import os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST_ARCH = "/usr/include/x86_64-linux-gnu"
GCC = "13"

def patch(path, old, new, regex=False):
    s = open(path).read()
    n = re.subn(old, new, s, flags=re.S) if regex else (s.replace(old, new), s.count(old))
    if n[1] == 0:
        sys.exit("make_overlay: pattern not found in %s: %r" % (path, old[:60]))
    open(path, "w").write(n[0])

def main():
    arch, cxx = os.path.join(HERE, "archinc"), os.path.join(HERE, "cxxarch")
    for d in (arch, cxx):
        shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(HOST_ARCH, arch, ignore=shutil.ignore_patterns("c++"))
    shutil.copytree(os.path.join(HOST_ARCH, "c++", GCC), cxx)

    # word size from the target, not from __x86_64__
    open(os.path.join(arch, "bits/wordsize.h"), "w").write(
        "/* overlay: ARM targets */\n#if defined __LP64__\n# define __WORDSIZE 64\n# define __WORDSIZE_TIME64_COMPAT32 0\n"
        "# define __SYSCALL_WORDSIZE 64\n#else\n# define __WORDSIZE 32\n# define __WORDSIZE32_SIZE_ULONG 0\n"
        "# define __WORDSIZE32_PTRDIFF_LONG 0\n# define __WORDSIZE_TIME64_COMPAT32 0\n#endif\n")
    open(os.path.join(arch, "gnu/stubs.h"), "w").write("/* overlay: no stubs */\n")
    # no _Float128 on 32-bit ARM; keep it off everywhere
    patch(os.path.join(arch, "bits/floatn.h"),
          r"#if \(defined __x86_64__.*?# define __HAVE_FLOAT128 1\n#else\n# define __HAVE_FLOAT128 0\n#endif",
          "#define __HAVE_FLOAT128 0", regex=True)
    # ARM hard-float evaluates float in float
    patch(os.path.join(arch, "bits/flt-eval-method.h"),
          "#else\n# define __GLIBC_FLT_EVAL_METHOD\t2\n#endif", "#else\n# define __GLIBC_FLT_EVAL_METHOD\t0\n#endif")
    # keep pre-2.38 scanf/strtol symbols (no __isoc23_* requirements)
    shutil.copy("/usr/include/features.h", os.path.join(arch, "features.h"))
    patch(os.path.join(arch, "features.h"), r"(#if[^\n]*\n# define __GLIBC_USE_C2X_STRTOL) 1", r"\1 0  /* overlay: keep pre-2.38 scanf/strtol symbols */", regex=True)
    # instantiate std::string etc. in the plugin (there is no libstdc++.so to link against)
    patch(os.path.join(cxx, "bits/c++config.h"), "# define _GLIBCXX_EXTERN_TEMPLATE 1", "# define _GLIBCXX_EXTERN_TEMPLATE 0")

    for a in ("arm32", "arm64"):
        subprocess.check_call([sys.executable, os.path.join(HERE, "mkstubs.py"), a, os.path.join(HERE, "stubs", a)])
    b = os.path.join(HERE, "bin")
    os.makedirs(b, exist_ok=True)
    for name in ("arm-linux-gnueabihf-g++", "aarch64-linux-gnu-g++"):
        p = os.path.join(b, name)
        if os.path.lexists(p):
            os.remove(p)
        os.symlink("../xcxx.py", p)
    os.chmod(os.path.join(HERE, "xcxx.py"), 0o755)
    print("make_overlay: ready. Build with:\n  PATH=%s:$PATH python3 tools/build.py plugins/YS4OP --toolchain gcc" % b)

if __name__ == "__main__":
    main()
