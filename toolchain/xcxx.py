#!/usr/bin/env python3
"""Stand-in for arm-linux-gnueabihf-g++ / aarch64-linux-gnu-g++ using the system clang + lld,
host libstdc++ headers (header-only use), a patched glibc arch-header overlay and stub libraries.
Exceptions/RTTI are off; a tiny runtime (rt/rt.cpp) supplies operator new/delete and friends."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.realpath(__file__))
name = os.path.basename(sys.argv[0])
arch = "arm32" if "gnueabihf" in name else "arm64"
triple = {"arm32": "armv7a-linux-gnueabihf", "arm64": "aarch64-linux-gnu"}[arch]
cpu = {"arm32": ["-mcpu=cortex-a17", "-mfpu=neon-vfpv4", "-mfloat-abi=hard"], "arm64": ["-march=armv8-a", "-mtune=cortex-a55"]}[arch]
args = [a for a in sys.argv[1:] if a not in ("-static-libstdc++", "-static-libgcc") and not a.startswith("-march=")
        and not a.startswith("-mfpu=") and not a.startswith("-mfloat-abi=")]
if "--version" in args or "-v" in args and len(args) == 1:
    print("xcxx (clang cross wrapper) for", triple); sys.exit(0)
inc = ["-nostdinc", "-nostdinc++",
       "-isystem", os.path.join(HERE, "cxxarch"), "-isystem", "/usr/include/c++/13",
       "-isystem", "/usr/include/c++/13/backward",
       "-isystem", "/usr/lib/llvm-18/lib/clang/18/include",
       "-isystem", os.path.join(HERE, "archinc"), "-isystem", "/usr/include"]
flags = ["--target=" + triple] + cpu + ["-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
         "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-U_FORTIFY_SOURCE",
         "-D_FORTIFY_SOURCE=0", "-fno-plt"]
compile_only = "-c" in args or "-E" in args or "-S" in args
cmd = ["clang++"] + flags + inc + args
if not compile_only:
    stub = os.path.join(HERE, "stubs", arch)
    rt_obj = os.path.join(HERE, "rt", "rt-%s.o" % arch)
    if not os.path.exists(rt_obj) or os.path.getmtime(rt_obj) < os.path.getmtime(os.path.join(HERE, "rt", "rt.cpp")):
        subprocess.check_call(["clang++"] + flags + inc + ["-std=c++17", "-O2", "-fPIC", "-fvisibility=hidden", "-c",
                               os.path.join(HERE, "rt", "rt.cpp"), "-o", rt_obj])
    cmd += ["-nostdlib", "-fuse-ld=lld", "-Wl,-z,defs", "-Wl,--as-needed", "-Wl,--hash-style=both", "-Wl,-z,noexecstack",
            "-Wl,-z,relro", "-Wl,-z,now", rt_obj,
            os.path.join(stub, "libm.so.6"), os.path.join(stub, "libc.so.6"),
            os.path.join(stub, "libdl.so.2"), os.path.join(stub, "libgcc_s.so.1")]
if os.environ.get("XCXX_VERBOSE"):
    sys.stderr.write(" ".join(cmd) + "\n")
sys.exit(subprocess.call(cmd))
