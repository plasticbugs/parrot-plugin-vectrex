// cvheap.c -- see cvheap.h.
#include "cvheap.h"

#include <errno.h>
#include <stdlib.h>

#include "cvlaunch.h"
#include "pico.h"                          // panic()

void *_sbrk(int incr) {
    extern char end;                       // .bss's end (the linker's)
    static char *top;
    if (!top) top = &end;
    char *prev = top;
    if (incr > 0 ? (uintptr_t)top + (uint32_t)incr > CVL_ADDR : top + incr < &end) {
        errno = ENOMEM;
        return (void *)-1;
    }
    top += incr;
    return prev;
}

uint32_t cvheap_free(void) {
    char *p = _sbrk(0);
    return p == (char *)-1 ? 0 : (uint32_t)(CVL_ADDR - (uintptr_t)p);
}

// malloc itself, under the SDK's wrapper (which panics when there's none)
extern void *__real_malloc(size_t n);

// newlib's malloc, the first time it grows the heap, asks _sbrk for the
// request and 16 bytes, then for what takes the break up to a 4K boundary;
// a request that ends in the last whole 4K below the launch block gets it
// all. (Later ones are rounded up to 4K first, and never ask for less than
// the whole request.) Should another malloc want it otherwise, the next
// size down is tried: a refused _sbrk changes nothing.
uint32_t cvheap_reserve(void) {
    extern char end;
    const uintptr_t top = CVL_ADDR & ~(uintptr_t)0xFFF;
    if (top < (uintptr_t)&end + 8192) return 0;
    for (uint32_t n = (uint32_t)(top - (uintptr_t)&end) - 64; n >= 4096; n -= 512) {
        void *p = __real_malloc(n);
        if (p) {
            free(p);
            return n;
        }
    }
    return 0;
}

uint32_t cvheap_largest(void) {
    extern char end;
    uint32_t lo = 0, hi = CVL_ADDR - (uintptr_t)&end;
    while (hi - lo > 8) {
        uint32_t mid = lo + (hi - lo) / 2;
        void *p = __real_malloc(mid);
        if (p) {
            free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// A pure virtual call is a bug: stop as a panic does (cvdiag reports it).
// Defined here so that libstdc++'s isn't linked: its std::terminate() brings
// in the exception runtime, and that takes an emergency pool of about 20K
// from this heap at start-up, exceptions or not. (NES 0.3.1: Mega Man 2's
// tile cache then didn't fit, and malloc's panic ended it.)
void __cxa_pure_virtual(void) {
    panic("pure virtual call");
}
