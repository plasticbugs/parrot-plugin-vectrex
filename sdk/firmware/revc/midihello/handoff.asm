;=========================================================================
; handoff.asm - the ROM the console runs while the cart changes firmware:
; from the SD menu (sdplayer) to PSG Synth or a plugin, and back.
;
; The cart can't be in both at once (sdplayer's USB is a device, PSG
; Synth's a host), so it switches by starting the other firmware, and for
; some milliseconds serves nothing (the data lines read $00). The console
; must not run code from the cart then. So the firmware about to stop
; serves this ROM first; the BIOS boots it (header 55 AA); it blanks the
; screen, silences the PSG and parks in console RAM, reading where every
; ColecoVision ROM keeps its title string ($8024):
;
;   "SD C..."  the menu (sdplayer's loader.asm)
;   "PSG ..."  PSG Synth (midihello.asm)
;   ...        a plugin's ROM: any title (docs/plugins.md)
;
; Once the same 4 printable characters (ASCII 32-126) have been there 32
; reads in a row, and they aren't this ROM's own ("HAND"), it restarts the
; BIOS (jp 0), which boots that ROM as at power-on. While the cart serves
; nothing they read $00, and past this ROM's code $C7: neither is printable.
;
;   sjasmplus --nologo handoff.asm -> handoff.bin (the code: both
;   firmwares serve it with RST 0, $C7, everywhere past it)
;=========================================================================
                DEVICE  NOSLOT64K

PSG             EQU     0xFF
VDP_CTRL        EQU     0xBF
TITLE           EQU     0x8024          ; the title string, in every one of these ROMs
PARK            EQU     0x7000          ; console RAM
STACK           EQU     0x7400
HITS            EQU     32

                ORG     0x8000
                DB      0x55,0xAA               ; skip the BIOS title screen
                DW      0,0,0,0
                DW      start
                DUP     7
                ret
                nop
                nop
                EDUP
                retn                            ; NMI ($8021)
                nop
                ASSERT  $ == TITLE
                DB      "HANDOFF/SD CART/2026",0

start:          di
                ld      sp,STACK
                in      a,(VDP_CTRL)            ; reset the VDP's write latch
                ld      a,0x80                  ; R1: 16K, blanked, no NMI
                out     (VDP_CTRL),a
                ld      a,0x81
                out     (VDP_CTRL),a
                ld      hl,silence
                ld      bc,4<<8 | PSG
                otir
                ld      hl,park_src
                ld      de,PARK
                ld      bc,park_end - park_src
                ldir
                jp      PARK

silence:        DB      0x9F,0xBF,0xDF,0xFF     ; all four PSG channels off

park_src:
                DISP    PARK
park:           ld      hl,TITLE                ; a title to wait on: 4 printable
                ld      de,seen                 ; characters, not "HAND"
                ld      b,4
.copy:          ld      a,(hl)
                cp      0x20
                jr      c,park
                cp      0x7F
                jr      nc,park
                ld      (de),a
                inc     hl
                inc     de
                djnz    .copy
                ld      hl,seen
                ld      de,own
                call    same4
                jr      z,park
                ld      c,HITS
.read:          ld      hl,TITLE                ; the same, HITS reads in a row
                ld      de,seen
                call    same4
                jr      nz,park                 ; it changed: start again
                dec     c
                jr      nz,.read
                jp      0x0000                  ; the BIOS: it boots the new ROM

same4:          ld      b,4                     ; Z if the 4 bytes at HL are those at DE
.c:             ld      a,(de)
                cp      (hl)
                ret     nz
                inc     hl
                inc     de
                djnz    .c
                ret

own:            DB      "HAND"
seen:           DS      4
                ENT
park_end:
                ASSERT  park_end - park_src < 0x300

                SAVEBIN "handoff.bin",0x8000,park_end-0x8000
