#!/usr/bin/env python3
"""Build link-time stub libraries (libc.so.6, libm.so.6, libdl.so.2, libgcc_s.so.1) for an ARM target.
They only carry symbol names + glibc symbol versions so the plugin links with correct versioned
references; at run time the MPC's real libraries are used."""
import os, subprocess, sys
arch, out = sys.argv[1], sys.argv[2]
BASE = {"arm32": "GLIBC_2.4", "arm64": "GLIBC_2.17"}[arch]
TRIPLE = {"arm32": "armv7a-linux-gnueabihf", "arm64": "aarch64-linux-gnu"}[arch]
LIBC_FUNCS = """malloc free calloc realloc posix_memalign aligned_alloc memalign memcpy memmove memset memcmp memchr
strlen strnlen bcmp strcmp strncmp strchr strrchr strcpy strncpy strcat strncat strstr strdup strtod strtof strtol strtoul
strtoll strtoull atoi atof abs labs snprintf vsnprintf sprintf vsprintf sscanf vsscanf printf fprintf vfprintf puts fputs
fputc putchar fopen fclose fread fwrite fgets fgetc fseek ftell rewind fflush ferror feof getcwd getenv abort exit
__cxa_atexit __cxa_finalize qsort bsearch rand srand time clock clock_gettime usleep nanosleep sysconf
pthread_self pthread_once pthread_mutex_lock pthread_mutex_unlock pthread_mutex_init pthread_mutex_destroy
pthread_key_create pthread_getspecific pthread_setspecific __errno_location open close read write lseek
readlink access isatty strerror __assert_fail __stack_chk_fail""".split()
LIBC_DATA = "stderr stdout stdin".split()
LIBDL = "dladdr dlopen dlsym dlclose dlerror".split()
LIBM_BASE = """sinf cosf tanf asinf acosf atanf atan2f sinhf coshf tanhf expf exp2f expm1f logf log2f log10f log1pf
powf sqrtf cbrtf floorf ceilf roundf truncf rintf lrintf lroundf llroundf fmodf fabsf hypotf ldexpf frexpf modff
fminf fmaxf sincosf sin cos tan asin acos atan atan2 sinh cosh tanh exp exp2 expm1 log log2 log10 log1p pow sqrt cbrt
floor ceil round trunc rint lrint lround llround fmod fabs hypot ldexp frexp modf fmin fmax sincos""".split()
# Functions that gained faster default versions after the base version (both exist on any glibc >= 2.29).
LIBM_NEW = {"expf": "GLIBC_2.27", "exp2f": "GLIBC_2.27", "logf": "GLIBC_2.27", "log2f": "GLIBC_2.27",
            "powf": "GLIBC_2.27", "exp": "GLIBC_2.29", "exp2": "GLIBC_2.29", "log": "GLIBC_2.29",
            "log2": "GLIBC_2.29", "pow": "GLIBC_2.29"}
GCC = {"arm32": "__aeabi_uldivmod __aeabi_ldivmod __aeabi_l2f __aeabi_ul2f __aeabi_l2d __aeabi_ul2d __aeabi_f2lz __aeabi_f2ulz __aeabi_d2lz __aeabi_d2ulz __aeabi_llsl __aeabi_llsr __aeabi_lasr __aeabi_lmul __aeabi_idiv __aeabi_uidiv __aeabi_idivmod __aeabi_uidivmod __popcountsi2 __clzdi2 __ctzdi2".split(),
       "arm64": "__popcountdi2 __clzti2".split()}
GCCVER = {"arm32": "GCC_3.5", "arm64": "GCC_3.0"}[arch]

def build(soname, groups, data=()):
    order = sorted(groups.keys(), key=lambda v: [int(x) for x in v.split("_")[1].split(".")])
    vers_of = {}
    for v in order:
        for sym in groups[v]:
            vers_of.setdefault(sym, []).append(v)
    src, script, prev = [], [], None
    for v in order:
        plain = [x for x in groups[v] if len(vers_of[x]) == 1]
        body = "global: " + " ".join(x + ";" for x in plain) + " " if plain else ""
        if prev is None:
            body += "local: *; "
        script.append("%s { %s}%s;" % (v, body, (" " + prev) if prev else ""))
        prev = v
    for sym, vs in vers_of.items():
        if len(vs) == 1:
            src.append(("void* %s = 0;" if sym in data else "void %s(void) {}") % sym)
        else:
            for i, v in enumerate(vs):
                a = "%s_v%d" % (sym, i)
                src.append("void %s(void) {}" % a)
                src.append('__asm__(".symver %s,%s%s%s");' % (a, sym, "@@" if i == len(vs) - 1 else "@", v))
    cfile = os.path.join(out, soname + ".c"); vfile = os.path.join(out, soname + ".map")
    open(cfile, "w").write("\n".join(src) + "\n")
    open(vfile, "w").write("\n".join(script) + "\n")
    cmd = ["clang", "--target=" + TRIPLE, "-w", "-fno-builtin", "-nostdlib", "-shared", "-fPIC", "-fuse-ld=lld", "-O0",
           "-Wl,-soname," + soname, "-Wl,--version-script=" + vfile, "-o", os.path.join(out, soname), cfile]
    if arch == "arm32":
        cmd[1:1] = ["-mfloat-abi=hard", "-mfpu=neon-vfpv4"]
    subprocess.check_call(cmd)

os.makedirs(out, exist_ok=True)
libc = {BASE: LIBC_FUNCS + LIBC_DATA}
iso = ["__isoc99_sscanf", "__isoc99_vsscanf", "__isoc99_scanf", "__isoc99_fscanf"]
if arch == "arm32": libc["GLIBC_2.7"] = iso
else: libc[BASE] = libc[BASE] + iso
build("libc.so.6", libc, data=LIBC_DATA)
build("libdl.so.2", {BASE: LIBDL})
m = {BASE: list(LIBM_BASE)}
for s, v in LIBM_NEW.items():
    m.setdefault(v, []).append(s)
build("libm.so.6", m)
build("libgcc_s.so.1", {GCCVER: GCC[arch]})
print("stubs ok:", out)
