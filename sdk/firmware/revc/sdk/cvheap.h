// cvheap.h -- a heap for a plugin that needs one (an emulator core that
// uses new and malloc), linked by cvcart_plugin(... HEAP). The SDK's _sbrk
// (pico_clib_interface, weak) lets the heap grow from .bss's end to the top
// of SRAM, through the launch block and the handoff trace (cvlaunch.h);
// cvheap.c's stops it below them, so malloc returns NULL instead.
#pragma once
#include <stdint.h>

// bytes the heap can still grow by (malloc's own free lists aside)
uint32_t cvheap_free(void);

// the largest block malloc() could still give, found by trying (a few
// dozen mallocs, each freed again): the number that says whether one more
// allocation fits, which cvheap_free() alone doesn't (malloc takes the
// heap from _sbrk 4K at a time, and keeps what's freed). Exact after
// cvheap_reserve(); without it a lower bound (a try that grows the heap
// leaves less for _sbrk to give the next, larger one).
uint32_t cvheap_largest(void);

// The whole heap taken from _sbrk now, as one free block, so that every
// later malloc() is cut from it: call it once, before the first malloc().
// Without it newlib's malloc grows the heap as it goes, each time by the
// whole request rounded up to 4K whatever it has free at the top, and a
// request the heap had room for is refused (NES 0.3.x: 16.5K asked for, 7.5K
// free at the top and 19K beyond it, and _sbrk asked for 20K). Returns the
// block's size. malloc ends its heap on a 4K boundary, so the part of a 4K
// above the last one below the launch block stays out of it.
uint32_t cvheap_reserve(void);
