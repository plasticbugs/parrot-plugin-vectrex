// mailbox.h -- what the cart and the kernel (midihello.asm) agree on. The
// kernel has the same addresses as EQUs; test/run.sh checks the two agree.
//
// The cart writes, the Z80 reads (it is ROM to the console):
//   $A000-$FEFF  LIST      a VDP write list (vdp.h), when LIST_SEQ changes
//   $FF00        SND_SEQ   changes whenever SND, NSEQ or NCTL does
//   $FF01-$FF0A  SND       10 PSG bytes: tone low, tone high, volume for
//                          channels 0-2, then the noise volume
//   $FF0B        NSEQ      changes when NCTL is to be written (that restarts
//                          the noise)
//   $FF0C        NCTL      the noise control byte
//   $FF10        LIST_SEQ  a new list is ready
//   $FF11        LIST_FULL nonzero: it is a whole screen (as against VRAM
//                          that vdp_init() describes), not changes
//
// The Z80 tells the cart things by reading $FF80-$FFFF, once a frame; the
// address is the message, and the cart's bus loop notes it:
//   $FF80 | seq   ACK      the last list it ran (LIST_SEQ & 31)
//   $FFA0 | seq   HELLO    it has started and run no list yet: VRAM is as
//                          vdp_init() says, and the screen must be sent whole.
//                          seq is the last list it took as done unrun: the
//                          LIST_SEQ it found at start, or a list of changes
//   $FFC0 | ...   the joystick, and $FFE0 | ... the keypad (pad.h)
//
// The cart writes the bytes first and the sequence numbers last. It writes
// a new list only once the Z80 has run the last one (ACK) or will never run
// it (HELLO with its seq), so the Z80 never reads a list being written
// (screen.c). Until the Z80 has run a list since its start it runs only a
// whole screen; a list of changes it takes as done without running it.
#pragma once

#define MB_BASE      0xFF00
#define MB_SND_SEQ   0x00
#define MB_SND       0x01
#define MB_SND_LEN   10
#define MB_NSEQ      0x0B
#define MB_NCTL      0x0C
#define MB_LIST_SEQ  0x10
#define MB_LIST_FULL 0x11
#define MB_SIZE      0x80       // $FF00-$FF7F

#define LIST_BASE    0xA000
#define LIST_SIZE    0x5F00     // to $FEFF

#define REPORT_ACK   0xFF80
#define REPORT_HELLO 0xFFA0
#define REPORT_BASE  0xFF80     // the cart's bus loop watches $FF80-$FFFF:
                                // kind = A5 | A6 << 1 (REPORT_KIND_*)
enum { REPORT_KIND_ACK, REPORT_KIND_HELLO, REPORT_KIND_JOY, REPORT_KIND_KEY };
