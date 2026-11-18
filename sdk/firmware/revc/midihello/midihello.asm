;=========================================================================
; midihello.asm - PSG Synth's console side.
;
; The cart is a USB host and a MIDI keyboard plugs into its USB-C. The
; cart's core 0 plays the keyboard on a synth of its own (synth.c), draws
; the screen (ui.c), and writes both into a mailbox inside the image core 1
; serves (mailbox.h has the map). To the Z80 the mailbox is ROM that
; changes. This kernel only carries things out:
;
;   - sound: the main loop polls SND_SEQ without pause and sends the 10 PSG
;     bytes as soon as it changes (and the noise control, only when NSEQ
;     changes: writing it restarts the noise);
;   - screen: Graphics II as a bitmap (name i is tile i). When LIST_SEQ
;     changes it runs the VDP write list at LIST (vdp.h), a byte each 30 T
;     or more (slow enough for the VDP during the active display), and
;     checks the sound between commands. Until it has run one, it runs
;     only a whole screen (LIST_FULL): a list of changes made for a screen
;     from before this start is taken as done, unrun;
;   - once a frame it reports to the cart by reading ROM at addresses that
;     carry the message: the joystick and keypad of controller 1 (pad.h),
;     then the last list it ran (ACK), or HELLO until it has run one (with
;     the LIST_SEQ it found at start, which it never runs: the cart may have
;     written that list for a screen this start doesn't have).
;
; At start it clears VRAM, sets the names and turns sprites off, which is
; the state vdp_init() describes; the cart sends the whole screen on HELLO.
;
;   sjasmplus --nologo midihello.asm -> mhkernel.bin (32K, $8000-$FFFF)
;=========================================================================
                DEVICE  NOSLOT64K

PSG             EQU     0xFF
VDP_DATA        EQU     0xBE
VDP_CTRL        EQU     0xBF
JOY_MODE        EQU     0xC0            ; out: controllers read the joystick
KEY_MODE        EQU     0x80            ; out: controllers read the keypad
CTRL1           EQU     0xFC
STACK_TOP       EQU     0x7400
FRAMES          EQU     0x7000          ; NMIs so far
SEEN            EQU     0x7001          ; FRAMES when the last frame started
SND_SENT        EQU     0x7002          ; SND_SEQ last sent
NSEQ_SENT       EQU     0x7003          ; NSEQ last sent
LIST_DONE       EQU     0x7004          ; LIST_SEQ last run
LIST_RUN        EQU     0x7005          ; LIST_SEQ being run
HELLO           EQU     0x7006          ; nonzero until a list has run

SND_SEQ         EQU     0xFF00
SND             EQU     0xFF01
SND_LEN         EQU     10
NSEQ            EQU     0xFF0B
NCTL            EQU     0xFF0C
LIST_SEQ        EQU     0xFF10
LIST_FULL       EQU     0xFF11          ; nonzero: that list is a whole screen
LIST            EQU     0xA000
REPORT_ACK      EQU     0xFF80          ; | LIST_DONE & 31
REPORT_HELLO    EQU     0xFFA0          ; | LIST_DONE & 31
JOY_REPORT      EQU     0xFFC0          ; | lines | fire << 4
KEY_REPORT      EQU     0xFFE0
LIST_END        EQU     0xFF

                ORG     0x8000
                DB      0x55,0xAA               ; skip the BIOS title screen
                DW      0,0,0,0
                DW      start
                DUP     7
                ret
                nop
                nop
                EDUP
                jp      nmi                     ; $8021
                DB      "PSG SYNTH/SD CART/2026",0

; vertical blank: count it. No port reads here: a status read between the
; two halves of a VRAM address would wreck the address.
nmi:            push    af
                ld      a,(FRAMES)
                inc     a
                ld      (FRAMES),a
                pop     af
                retn

start:          di
                ld      sp,STACK_TOP
                in      a,(VDP_CTRL)            ; reset the VDP's write latch
                ld      hl,regs                 ; Graphics II, blanked, no NMI
                ld      b,0x80
.reg:           ld      a,(hl)
                out     (VDP_CTRL),a
                ld      a,b
                out     (VDP_CTRL),a
                inc     hl
                inc     b
                bit     3,b
                jr      z,.reg
                ld      hl,silence
                ld      bc,4<<8 | PSG
                otir
                xor     a                       ; all 16K of VRAM to zero
                out     (VDP_CTRL),a
                ld      a,0x40
                out     (VDP_CTRL),a
                ld      d,64
                xor     a
                ld      b,a
.clr:           out     (VDP_DATA),a            ; fine while blanked
                djnz    .clr
                dec     d
                jr      nz,.clr
                xor     a                       ; names: 0-255 in each third
                out     (VDP_CTRL),a
                ld      a,0x38 | 0x40
                out     (VDP_CTRL),a
                ld      d,3
                xor     a
.name:          out     (VDP_DATA),a
                inc     a
                jr      nz,.name
                dec     d
                jr      nz,.name
                xor     a                       ; no sprites
                out     (VDP_CTRL),a
                ld      a,0x3B | 0x40
                out     (VDP_CTRL),a
                ld      a,0xD0
                out     (VDP_DATA),a
                ld      a,(SND_SEQ)             ; send whatever sound is there now
                cpl
                ld      (SND_SENT),a
                ld      a,(NSEQ)
                cpl
                ld      (NSEQ_SENT),a
                ld      a,(LIST_SEQ)            ; a list from before this start is stale
                ld      (LIST_DONE),a
                ld      a,1
                ld      (HELLO),a
                xor     a
                ld      (FRAMES),a
                ld      (SEEN),a
                in      a,(VDP_CTRL)            ; clear F
                ld      a,0xE0                  ; 16K, display on, vertical blank NMI on
                out     (VDP_CTRL),a
                ld      a,0x81
                out     (VDP_CTRL),a

main:           call    sound
                ld      a,(FRAMES)
                ld      hl,SEEN
                cp      (hl)
                call    nz,frame
                ld      a,(LIST_SEQ)
                ld      hl,LIST_DONE
                cp      (hl)
                call    nz,run_list
                jr      main

; the sound, if it changed
sound:          ld      a,(SND_SEQ)
                ld      hl,SND_SENT
                cp      (hl)
                ret     z
                ld      (hl),a                  ; sending this one; a change from now on is sent again
                ld      a,(NSEQ)                ; the cart writes NCTL before NSEQ, NSEQ before SND_SEQ
                ld      hl,NSEQ_SENT
                cp      (hl)
                jr      z,.snd
                ld      (hl),a
                ld      a,(NCTL)
                out     (PSG),a
.snd:           ld      hl,SND
                ld      bc,SND_LEN<<8 | PSG
                otir
                ret

; once a frame (A = FRAMES, HL = SEEN): the controller, then ACK or HELLO.
; Each report is read twice, so one read the cart misses doesn't lose it.
frame:          ld      (hl),a
                in      a,(VDP_CTRL)            ; F off, so the next vertical blank raises NMI
                out     (JOY_MODE),a
                ex      (sp),hl                 ; let the lines settle
                ex      (sp),hl
                in      a,(CTRL1)
                ld      hl,JOY_REPORT
                call    .pad
                out     (KEY_MODE),a
                ex      (sp),hl
                ex      (sp),hl
                in      a,(CTRL1)
                ld      hl,KEY_REPORT
                call    .pad
                ld      a,(LIST_DONE)
                and     0x1F
                ld      c,a
                ld      a,(HELLO)
                or      a
                ld      a,REPORT_ACK & 0xFF
                jr      z,.ack
                ld      a,REPORT_HELLO & 0xFF
.ack:           or      c
                ld      l,a
                ld      h,REPORT_ACK >> 8
.read:          ld      a,(hl)
                ld      a,(hl)
                ret
.pad:           ld      c,a                     ; bits 0-3 the lines, bit 6 the fire button
                and     0x0F
                or      l
                bit     6,c
                jr      z,.pressed
                or      0x10
.pressed:       ld      l,a
                jr      .read

; run the list at LIST (A = LIST_SEQ):
;   n (1-127), address, n bytes | $80+n, address, a byte to fill n with |
;   0, value, $80+register | $FF the end
run_list:       ld      (LIST_RUN),a
                ld      a,(HELLO)
                or      a
                jr      z,.run
                ld      a,(LIST_FULL)           ; just started: only a whole screen will do
                or      a
                jr      z,.skip
.run:           ld      hl,LIST
.cmd:           push    hl
                call    sound                   ; the sound waits one command at most
                pop     hl
                ld      a,(hl)
                inc     hl
                cp      LIST_END
                jr      z,.end
                ld      b,a
                ld      a,(hl)                  ; the address, or a register write
                out     (VDP_CTRL),a
                inc     hl
                ld      a,(hl)
                out     (VDP_CTRL),a
                inc     hl
                ld      a,b
                or      a
                jr      z,.cmd
                ld      c,VDP_DATA
                bit     7,b
                jr      nz,.fill
.copy:          outi                            ; 16 T
                nop                             ;  4
                jp      nz,.copy                ; 10: 30 T a byte, 8.4 us
                jr      .cmd
.fill:          res     7,b
                ld      a,(hl)
                inc     hl
.f:             out     (c),a                   ; 12 T
                nop                             ;  4
                nop                             ;  4
                djnz    .f                      ; 13: 33 T a byte
                jr      .cmd
.end:           xor     a
                ld      (HELLO),a
.skip:          ld      a,(LIST_RUN)
                ld      (LIST_DONE),a
                ret

regs:           DB      0x02                    ; 0: Graphics II
                DB      0x80                    ; 1: 16K, blanked, no NMI
                DB      0x0E                    ; 2: names at $3800
                DB      0xFF                    ; 3: colours at $2000, all of it
                DB      0x03                    ; 4: patterns at $0000, all of it
                DB      0x76                    ; 5: sprite attributes at $3B00
                DB      0x03                    ; 6: sprite patterns at $1800
                DB      0x01                    ; 7: black backdrop
silence:        DB      0x9F,0xBF,0xDF,0xFF     ; all four PSG channels off

                ASSERT  $ <= LIST
                SAVEBIN "mhkernel.bin",0x8000,0x8000
