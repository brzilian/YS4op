// Minimal C++ runtime for SDK plugins built without libstdc++/libsupc++.
// Exceptions and RTTI are disabled; anything that would throw aborts.
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#define RT_HIDDEN __attribute__((visibility("hidden")))
extern "C" {
RT_HIDDEN void* __dso_handle = &__dso_handle;
RT_HIDDEN void __cxa_pure_virtual() { abort(); }
// What crtbeginS.o normally does: when the library is unloaded (dlclose), run and unregister the
// destructors of its static objects, so nothing is left pointing into unmapped code.
void __cxa_finalize(void*);
__attribute__((destructor, visibility("hidden"))) void rt_run_static_destructors() { __cxa_finalize(&__dso_handle); }
RT_HIDDEN void __cxa_deleted_virtual() { abort(); }
// Thread-safe statics guard (Itanium ABI; ARM EABI uses a 32-bit guard word).
#if defined(__arm__)
typedef int32_t guard_t;
#else
typedef int64_t guard_t;
#endif
RT_HIDDEN int __cxa_guard_acquire(guard_t* g)
{
    volatile uint8_t* b = (volatile uint8_t*)g;
    for (;;) {
        if (__atomic_load_n(b, __ATOMIC_ACQUIRE) & 1) return 0;       // already initialised
        uint8_t expected = 0;
        if (__atomic_compare_exchange_n(b + 1, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(b, __ATOMIC_ACQUIRE) & 1) { __atomic_store_n(b + 1, 0, __ATOMIC_RELEASE); return 0; }
            return 1;                                                      // we initialise
        }
    }
}
RT_HIDDEN void __cxa_guard_release(guard_t* g)
{
    volatile uint8_t* b = (volatile uint8_t*)g;
    __atomic_store_n(b, 1, __ATOMIC_RELEASE);
    __atomic_store_n(b + 1, 0, __ATOMIC_RELEASE);
}
RT_HIDDEN void __cxa_guard_abort(guard_t* g) { __atomic_store_n(((volatile uint8_t*)g) + 1, 0, __ATOMIC_RELEASE); }
#if defined(__arm__)
RT_HIDDEN int __aeabi_atexit(void* obj, void (*fn)(void*), void* dso)
{ extern int __cxa_atexit(void (*)(void*), void*, void*); return __cxa_atexit(fn, obj, dso); }
#endif
}
namespace std {
RT_HIDDEN void __throw_length_error(const char*) { abort(); }
RT_HIDDEN void __throw_logic_error(const char*) { abort(); }
RT_HIDDEN void __throw_out_of_range(const char*) { abort(); }
RT_HIDDEN void __throw_out_of_range_fmt(const char*, ...) { abort(); }
RT_HIDDEN void __throw_bad_alloc() { abort(); }
RT_HIDDEN void __throw_bad_array_new_length() { abort(); }
RT_HIDDEN void __throw_invalid_argument(const char*) { abort(); }
RT_HIDDEN void __throw_bad_function_call() { abort(); }
RT_HIDDEN void __glibcxx_assert_fail(const char*, int, const char*, const char*) noexcept { abort(); }
struct nothrow_t;
enum class align_val_t : size_t;
}
void* operator new(size_t n) { void* p = malloc(n ? n : 1); if (!p) abort(); return p; }
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void* operator new(size_t n, std::align_val_t a)
{ void* p = nullptr; if (posix_memalign(&p, (size_t)a < sizeof(void*) ? sizeof(void*) : (size_t)a, n ? n : 1)) abort(); return p; }
void operator delete(void* p, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { free(p); }
