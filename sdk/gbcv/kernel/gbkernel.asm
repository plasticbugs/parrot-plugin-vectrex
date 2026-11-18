;=========================================================================
; gbkernel - the ColecoVision side of a Game Boy running on the rev C cart.
;
; The cart emulates the Game Boy and turns each picture into Graphics II
; tiles and the sound into PSG bytes. This kernel, served as the fixed bank
; at $8000, does one thing each TV frame: at the start of vertical blank it
; reports the controller to the cart, takes that frame's command list at
; $C000, and runs it.
;
; Frame start (NMI, then the kernel leaves its wait loop):
;   read $FFC0+dirs     joystick bits 0-3 as read (low = pressed)
;   read $FFE0+key      keypad nibble as read ($F = none)
;   read $FFD0+fires    bit 0 left fire, bit 1 right fire (low = pressed)
;   read $FFF0+seq      seq counts frames mod 16; the cart answers this read
;                       by putting the next list at $C000 (an empty one if
;                       it has none ready). Each list runs once.
;
; A list, at $C000:
;   vertical blank part, VRAM writes as fast as the Z80 goes
;     1 lo hi n d*n     VRAM run: address lo, hi|$40, n bytes (1-255)
;     2 n d*n           PSG bytes
;     3 v r             VDP register r ($80|r) = v
;     0                 end of the vertical blank part
;   active display part, VRAM writes 31 T apart (the console's floor is 28)
;     1 lo hi n d*n     VRAM run
;     2 n d*n           PSG bytes
;     0                 end of the list: wait for the next frame
;
; Costs in T, counting the ColecoVision's wait state on every M1 fetch, and
; ~29 T of PSG READY on each PSG write (gbcv/src/list.rs has them as
; constants, and the cart keeps every list inside its frame by them):
;   vertical blank:  run 106 + 23n, PSG 75 + 52n, register 114, end 54
;   active display:  run 106 + 31n, PSG 75 + 52n, end 52
;
; VRAM: patterns $0000, colours $2000, names $1800, sprites off ($1B00 =
; $D0). The cart never writes slot 0 of a third, which stays all zero:
; colour 0 shows the backdrop (R7), the border.
;
;   sjasmplus --nologo --sym=gbkernel.sym gbkernel.asm -> gbkernel.bin (16K)
;=========================================================================
                DEVICE  NOSLOT64K

PSG             EQU     0xFF
VDP_DATA        EQU     0xBE
VDP_CTRL        EQU     0xBF
KEY_MODE        EQU     0x80            ; out: the controllers read the keypad
JOY_MODE        EQU     0xC0            ; out: the controllers read the joystick
CTRL1           EQU     0xFC
STACK_TOP       EQU     0x7400
FRAMES          EQU     0x7000          ; NMIs so far
SEEN            EQU     0x7001          ; FRAMES when the last frame started
SEQ             EQU     0x7002          ; frames started
LIST            EQU     0xC000

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
                DB      "GAME BOY/SD CART/2026",0

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
                im      1
                call    psg_init
                call    vdp_init
                xor     a
                ld      (FRAMES),a
                ld      (SEEN),a
                ld      (SEQ),a
                in      a,(VDP_CTRL)            ; clear F
                ld      a,0xE0                  ; 16K, display on, vertical blank NMI on
                out     (VDP_CTRL),a
                ld      a,0x81
                out     (VDP_CTRL),a

frame:          ld      hl,FRAMES
                ld      a,(SEEN)
.wait:          cp      (hl)
                jr      z,.wait
                ld      a,(hl)
                ld      (SEEN),a
                in      a,(VDP_CTRL)            ; F off, so the next vertical blank raises NMI
                out     (JOY_MODE),a
                ld      h,0xFF
                in      a,(CTRL1)               ; joystick: bits 0-3, left fire bit 6
                ld      e,a
                out     (KEY_MODE),a
                and     0x0F
                or      0xC0
                ld      l,a
                ld      a,(hl)                  ; report the joystick
                in      a,(CTRL1)               ; keypad: bits 0-3, right fire bit 6
                ld      d,a
                and     0x0F
                or      0xE0
                ld      l,a
                ld      a,(hl)                  ; report the keypad
                ld      a,e
                rlca
                rlca                            ; left fire to bit 0
                and     1
                ld      e,a
                ld      a,d
                rlca
                rlca
                rlca                            ; right fire to bit 1
                and     2
                or      e
                or      0xD0
                ld      l,a
                ld      a,(hl)                  ; report the fire buttons
                ld      a,(SEQ)
                inc     a
                ld      (SEQ),a
                and     0x0F
                or      0xF0
                ld      l,a
                ld      a,(hl)                  ; the cart puts this frame's list at $C000
                ld      hl,LIST

; ---- vertical blank part -------------------------------------------------
vop:            ld      a,(hl)                  ; 8
                inc     hl                      ; 7
                dec     a                       ; 5
                jr      z,v_run                 ; 13 / 8
                dec     a
                jr      z,v_psg
                dec     a
                jr      z,v_reg
                ; 0: on to the active display part

; ---- active display part -------------------------------------------------
aop:            ld      a,(hl)
                inc     hl
                dec     a
                jr      z,a_run
                dec     a
                jr      z,a_psg
                jp      frame                   ; 0: the list is done

v_run:          ld      c,VDP_CTRL
                outi                            ; address low
                outi                            ; address high | $40
                ld      b,(hl)
                inc     hl
                ld      c,VDP_DATA
                otir                            ; 23 T a byte
                jp      vop

v_psg:          ld      b,(hl)
                inc     hl
                ld      c,PSG
                otir
                jp      vop

v_reg:          ld      c,VDP_CTRL
                outi
                outi
                jp      vop

a_run:          ld      c,VDP_CTRL
                outi
                outi
                ld      b,(hl)
                inc     hl
                ld      c,VDP_DATA
.byte:          outi                            ; 18
                jr      nz,.byte                ; 13: 31 T a byte
                jp      aop

a_psg:          ld      b,(hl)
                inc     hl
                ld      c,PSG
                otir
                jp      aop

; ---- setup ----------------------------------------------------------------
psg_init:       ld      a,0x9F
                out     (PSG),a
                ld      a,0xBF
                out     (PSG),a
                ld      a,0xDF
                out     (PSG),a
                ld      a,0xFF
                out     (PSG),a
                ret

vdp_init:       ld      hl,vdp_regs
                ld      b,16
                ld      c,VDP_CTRL
                otir
                xor     a                       ; clear all 16K
                out     (VDP_CTRL),a
                ld      a,0x40
                out     (VDP_CTRL),a
                ld      de,0x4000
.clr:           xor     a
                out     (VDP_DATA),a
                dec     de
                ld      a,d
                or      e
                jr      nz,.clr
                xor     a                       ; $1B00: no sprites
                out     (VDP_CTRL),a
                ld      a,0x1B | 0x40
                out     (VDP_CTRL),a
                ld      a,0xD0
                out     (VDP_DATA),a
                ret

; Graphics II, display off until the VRAM is clear; names $1800, colours
; $2000, patterns $0000, sprite attributes $1B00, sprite patterns $3800,
; backdrop black
vdp_regs:       DB      0x02,0x80, 0x80,0x81, 0x06,0x82, 0xFF,0x83
                DB      0x03,0x84, 0x36,0x85, 0x07,0x86, 0x01,0x87

                SAVEBIN "gbkernel.bin",0x8000,0x4000
