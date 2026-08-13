// cxxsupp.cpp - minimal C++ runtime SHARED by the MayteraOS C++ app ports.
//
// SHARED FILE (userland/libcompat). AssaultCube and OpenArena each carried a
// byte-identical private copy of this file until #745 promoted it here. Do
// NOT fork a private copy back into an app directory: extend THIS file, then
// go back and confirm every existing consumer still builds. Consumers today:
//   userland/apps/assaultcube, userland/apps/openarena
// userland/apps/curaslice keeps its own older, genuinely DIFFERENT variant
// (344 lines vs 114); it is not a duplicate of this file and was left alone.
//
// NAMING NOTE: the internal abort helper is still called assaultcube_die().
// #745 promoted this file with its code byte-identical on purpose, so that
// both ports binaries could be PROVEN unchanged by the move. Renaming that
// symbol is a separate cosmetic change, deliberately not bundled here.
//
// Adapted from userland/apps/curaslice/cxxsupp.cpp (the first C++ app in this
// tree), which established the pattern: the app is built -fno-exceptions
// -fno-rtti -fno-threadsafe-statics and linked -nostdlib against libc.a, so
// NONE of libstdc++'s runtime is present. This object supplies operator
// new/delete, the C++ ABI helpers (__cxa_*, __dso_handle), and the
// std::__throw_* family that AssaultCube's headers can reference even with
// exceptions disabled (Cube engine itself is close to STL-free, but a few
// vendored/system headers pulled in transitively are not).
//
// DELIBERATELY NOT copied from curaslice: its roundf() and qsort() gap-fillers.
// Those existed because curaslice's libc.a snapshot predated those routines;
// the current libc.a (userland/libc/math.c, stdlib.c) already provides both,
// so redefining them here would be a duplicate-symbol link error (this is
// exactly the error curaslice itself now hits on a fresh rebuild - a
// pre-existing, unrelated drift issue observed during this port's bring-up,
// not something introduced by this file).
//
// This list is a STARTING point, same protocol as curaslice: the authoritative
// set is "whatever the first real link against the full engine reports as
// undefined." See docs/ASSAULTCUBE_PORT_PLAN.md.
//
// No em-dashes per repo writing-style rule.

#include <stddef.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>

extern "C" void assaultcube_die(const char* msg) __attribute__((noreturn));
extern "C" void assaultcube_die(const char* msg)
{
    if (msg)
        printf("assaultcube: fatal: %s\n", msg);
    exit(1);
    for (;;) { }
}

// ---------------------------------------------------------------------------
// operator new / delete onto malloc / free. Allocation failure aborts (the
// app is -fno-exceptions, so we cannot throw std::bad_alloc).
// ---------------------------------------------------------------------------
void* operator new(size_t n)
{
    if (n == 0) n = 1;
    void* p = malloc(n);
    if (!p) assaultcube_die("operator new: out of memory");
    return p;
}
void* operator new[](size_t n)              { return operator new(n); }

void  operator delete(void* p)              { if (p) free(p); }
void  operator delete[](void* p)            { if (p) free(p); }

// C++14 sized-deallocation forms (gcc-12 emits these).
void  operator delete(void* p, size_t)      { if (p) free(p); }
void  operator delete[](void* p, size_t)    { if (p) free(p); }

// nothrow forms.
void* operator new(size_t n, const std::nothrow_t&) throw()   { if (n == 0) n = 1; return malloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) throw() { if (n == 0) n = 1; return malloc(n); }
void  operator delete(void* p, const std::nothrow_t&) throw()   { if (p) free(p); }
void  operator delete[](void* p, const std::nothrow_t&) throw() { if (p) free(p); }

// ---------------------------------------------------------------------------
// C++ ABI helpers.
// ---------------------------------------------------------------------------
extern "C" {

void __cxa_pure_virtual() { assaultcube_die("pure virtual function called"); }

// Static-object destructor registration. We never run global dtors on exit
// (the process just ends), so record nothing and report success.
int __cxa_atexit(void (*/*fn*/)(void*), void* /*arg*/, void* /*dso*/) { return 0; }

// Handle for this "shared object"; the ABI wants the address, never the
// value.
void* __dso_handle = 0;

} // extern "C"

// ---------------------------------------------------------------------------
// std::terminate and the verbose terminate handler.
// ---------------------------------------------------------------------------
namespace std {
void terminate() { assaultcube_die("std::terminate"); }
} // namespace std

namespace __gnu_cxx {
void __verbose_terminate_handler() { assaultcube_die("terminate handler"); }
} // namespace __gnu_cxx

// ---------------------------------------------------------------------------
// std::__throw_* family, in case any transitively-included header (not Cube's
// own code, which avoids STL) reaches for bounds-checked containers.
// ---------------------------------------------------------------------------
namespace std {
void __throw_length_error(const char* s)          { assaultcube_die(s ? s : "length_error"); }
void __throw_out_of_range(const char* s)          { assaultcube_die(s ? s : "out_of_range"); }
void __throw_out_of_range_fmt(const char* s, ...) { assaultcube_die(s ? s : "out_of_range"); }
void __throw_bad_alloc()                          { assaultcube_die("bad_alloc"); }
void __throw_logic_error(const char* s)           { assaultcube_die(s ? s : "logic_error"); }
void __throw_bad_function_call()                  { assaultcube_die("bad_function_call"); }
void __throw_invalid_argument(const char* s)      { assaultcube_die(s ? s : "invalid_argument"); }
void __throw_runtime_error(const char* s)         { assaultcube_die(s ? s : "runtime_error"); }
void __throw_bad_cast()                           { assaultcube_die("bad_cast"); }
void __throw_bad_array_new_length()               { assaultcube_die("bad array new length"); }
} // namespace std

// ---------------------------------------------------------------------------
// POSIX stubs the upstream engine references on the Linux/Unix build path.
// ---------------------------------------------------------------------------
extern "C" int setpriority(int /*which*/, int /*who*/, int /*prio*/) { return 0; }
