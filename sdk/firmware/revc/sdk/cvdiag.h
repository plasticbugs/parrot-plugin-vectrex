// cvdiag: where a plugin got to, kept through a reboot into the menu, which
// prints it with its handoff trace ('t' on its USB serial; ../sdplayer/htrace.h).
// For a plugin with the Game Boy kernel's lists (cvframes.h).
//
//   cvd_start()          after cvf_start(): a fault handler, and a sampler
//                        of core 0 20 times a second
//   cvd_stage(n)         a milestone
//   cvd_stack(base, n)   a stack of the plugin's own to watch, painted now
//                        (before it's in use); cvd_stack_used(): its depth so far
//
// Each is a mark in the plugin's boot phase, which the menu prints as "?"
// and its value: four kept in place, the sampler's PC, LR and lowest SP,
// then the last stage's n; after a fault, its PC, LR, CFSR, fault address
// and SP. A fault goes back to the menu at once, its note "fault pc...
// cfsr...". Past a stack limit (MSPLIM) on cvd_stack()'s stack, the note is
// "stack over <a> <b>" (without cvd_stack(): "stack sp... pc...") and the marks four return addresses and the SP: the
// ones most often in the deepest 4K, a recursion's (addr2line them; the
// limit wants 200 bytes or so below it for this). A hang the watchdog will
// catch notes "hung pc..." a second before it does. A panic() (out of
// memory, a hard assert, TinyUSB's host) notes "panic <its message> <its
// caller>" and goes back to the menu; its marks are the caller, the
// message's address and the heap left. The menu shows 30 characters of a
// note: each of these fits (a panic's message is cut to 15).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void cvd_start(void);
void cvd_stage(uint32_t n);
void cvd_stack(void *base, uint32_t bytes);
uint32_t cvd_stack_used(void);

#ifdef __cplusplus
}
#endif
