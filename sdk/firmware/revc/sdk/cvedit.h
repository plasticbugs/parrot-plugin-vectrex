// cvedit.h -- a plain text editor for a plugin to embed: typed on a USB
// keyboard (cvusb.h's KEYBOARD), drawn in cvfont.h's 6x8 font with word
// wrap, loading and saving files on the card through fsro. The Text Editor
// plugin (../textedit) is a thin wrapper around it; a plugin that runs
// scripts can use it to write them on the console.
//
//   static cvedit_t ed;                    // about 12K: static, not on the stack
//   static uint8_t text[65536];
//   cvedit_config_t cfg = {.text = text, .cap = sizeof text, .x = 8, .y = 0, .cols = 40, .rows = 24,
//                          .ext = ".txt", .home = "/TEXT", .stem = "NOTE",
//                          .keys = "1 SAVE  2 OPEN  3 NEW", .card = my_card};
//   cvedit_init(&ed, &cfg);                // an empty, unnamed text
//   cvedit_load(&ed, "/TEXT/A.TXT", NULL); // or a file
//
//   each pass:   while (cvusb_key(&k)) ev = cvedit_key(&ed, &k);
//                ev = cvedit_task(&ed, time_us_32());       // key repeat, a save that is due
//   each frame:  ev = cvedit_pad(&ed, cvin_pressed(&in), cvin_key(&in));
//                cvs_update(...) with a draw function that calls cvedit_draw(&ed, canvas, all)
//
// Everything is in the cvedit_t: there are no globals, so a host can leave
// the editor (to run the script), draw what it likes on the canvas, and come
// back: call cvedit_redraw(), or cvedit_init() again to start over.
//
// The screen, in the region the host gives (x, y in pixels; cols cells of 6
// pixels, rows lines of 8):
//
//   line 0        the file's name, a * while there are unsaved changes, and
//                 the line the cursor is on (the file's lines, from 1)
//   lines 1..     the text, word-wrapped at cols. The wrap is on the screen
//                 only: a paragraph stays one line in the file.
//   the last      the keys (cfg.keys), or a message, or a question
//
// The region is cols * 6 + 6 pixels wide: one cell past the last column is
// where the cursor stands after a line that is exactly full. The top and
// bottom lines' colour runs 8 pixels further left, and right to the next
// multiple of 8.
//
// Keys: characters, Enter, Tab (kept as a tab, shown to the next 4th
// column), Backspace, Delete, the arrows, Home and End (the screen line; with
// Ctrl the text's start and end), Page Up and Down. A key held repeats.
// Ctrl+S saves, Ctrl+O opens, Ctrl+N is a new text. On the console's
// controller: keypad 1 saves, 2 opens, 3 is new, and the stick moves the
// cursor. Every other keypad key, Ctrl+letter and F-key is the host's: it
// comes back as an event (CVEDIT_HOST_*), while the text is being edited.
//
// Files: the bytes are kept. Lines end as the file's did (CRLF or LF; LF in a
// new text). A byte that isn't printable ASCII, a tab or a line end shows as
// '?' and is saved as it was. A file bigger than cfg.cap isn't opened.
//
// Open lists the files ending in cfg.ext in cfg.home, and in the folder the
// current file came from. A new text's first save asks for a name, offering
// the first free <stem>1<ext>, <stem>2<ext>... in cfg.home, which that save
// makes if the card doesn't have it (fs_mkdir; its parent must be there).
//
// Saving never leaves the card without the text, whenever the power goes:
//
//   1. the text goes into a spare file next to the target, EDITSAV1.TMP or
//      EDITSAV2.TMP, behind a header with the target's name and a CRC of it
//      all. The header's first sector is written last. It is read back.
//   2. the old file is deleted, the new one made at the new size, written
//      and read back.
//   3. the spare's header is wiped, and the spare deleted.
//
// Cut before 1 is done, the old file is whole and the spare's CRC is wrong
// (it is deleted when found). Cut later, the spare is whole: when a file is
// opened (or at the start) the editor looks in that folder and in cfg.home,
// says so, and offers to finish the save. Two spare names, so a save tried
// again after one that failed never wipes the only good copy.
//
// The card: cfg.card() is called before each thing done with it, and gives
// the mounted card with a writer (fs_set_writer), or NULL if there is none.
// failed says the last use hit an error (a card pulled out): mount it again.
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../midihello/vdp.h"
#include "../sdplayer/fsro.h"
#include "cvusb_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef fs_t *(*cvedit_card_fn)(void *ctx, bool failed);

typedef struct {
    uint8_t *text;              // the host's buffer for the text
    uint32_t cap;               // its size: the biggest file
    int x, y;                   // the region's top left, in pixels (x a multiple of 8 keeps the bars' colours right)
    int cols, rows;             // columns of text (10 to 42), and lines with the top and bottom ones (4 to 24)
    const char *ext;            // ".txt": what Open lists, and a new name's ending
    const char *home;           // "/TEXT": where Open looks and new files go
    const char *stem;           // "NOTE": a new file is offered NOTE1.TXT, NOTE2.TXT...
    const char *keys;           // the bottom line: "1 SAVE  2 OPEN  3 NEW"
    cvedit_card_fn card;
    void *ctx;                  // card()'s
} cvedit_config_t;

// What cvedit_key(), cvedit_pad() and cvedit_task() return.
enum {
    CVEDIT_NONE = 0,
    CVEDIT_SAVED,               // the text is on the card
    CVEDIT_SAVE_FAILED,         // it isn't (the bottom line says why)
    CVEDIT_OPENED,              // another text is in the editor: a file, a new one, a save finished
    CVEDIT_HOST_KEYPAD = 0x100, // + the keypad key: 0, 4-9, CVEDIT_STAR, CVEDIT_HASH
    CVEDIT_HOST_CTRL = 0x200,   // + the letter, in capitals: CVEDIT_HOST_CTRL + 'R'
    CVEDIT_HOST_FKEY = 0x300,   // + 1 to 12
};
enum { CVEDIT_STAR = 10, CVEDIT_HASH = 11 };
// cvedit_pad()'s pressed bits: cvinput.h's CVIN_*
enum { CVEDIT_UP = 1, CVEDIT_RIGHT = 2, CVEDIT_DOWN = 4, CVEDIT_LEFT = 8, CVEDIT_FIRE_L = 16, CVEDIT_FIRE_R = 32 };

#define CVEDIT_LIST_MAX 96      // files Open lists
#define CVEDIT_NAME_MAX 40      // characters of a name typed, with its ending

typedef struct {
    char name[CVEDIT_NAME_MAX + 1];     // as shown ('?' outside ASCII, cut to fit)
    uint32_t clus, size;
    uint8_t contig, where;      // where: 0 cfg.home, 1 the current file's folder
} cvedit_entry_t;

typedef struct {
    cvedit_config_t cfg;
    // the text
    uint32_t len, cursor, top;  // top: where the first line shown starts
    int goal;                   // the column Up and Down aim for (-1: the cursor's)
    uint32_t hint;              // a line start known to be right, for finding others (cvedit.c)
    uint32_t count_at, count;   // line ends before count_at
    bool crlf, modified;
    // its file: the folder ("" with no file), and the name (0 units: none yet)
    char dir[FS_LNAME];
    uint16_t name16[FS_LNAME];
    int name_n;
    char name[FS_NAME];         // for the top line
    // what the editor is doing
    uint8_t mode, ask;          // CVE_*, CMD_* (cvedit.c)
    bool redraw, drawn, save_due;
    uint32_t save_t, now;
    char msg[48];               // the bottom line, until the next key
    bool msg_bad;
    const char *top_text, *keys;        // cvedit_bars()
    // a key held
    cvkey_t held;
    uint32_t held_t;
    bool repeating;
    // Open's list
    cvedit_entry_t list[CVEDIT_LIST_MAX];
    int list_n, list_at, list_top;
    char list_dir[2][FS_LNAME];
    // the name being typed
    char typed[CVEDIT_NAME_MAX + 1];
    char save_dir[FS_LNAME];
    // a save found cut short: where, and which spare
    char found_dir[FS_LNAME];
    char found_name[FS_NAME];
    int found_slot;
    // a file to open once that is settled
    char wait_path[FS_LNAME];
    fs_file_t wait_file;
    bool wait, wait_has_file;
    // scratch
    fs_dir_t d;
    uint8_t sec[512], sec2[512];
    uint8_t hdr[1024];
    uint16_t tmp16[FS_LNAME];
    uint32_t saves, writes;     // saves done; sectors written by the last one
} cvedit_t;

// An empty text with no name. cfg is copied (its strings and buffer aren't).
void cvedit_init(cvedit_t *e, const cvedit_config_t *cfg);

// A key from cvusb_key(), down or up. Returns an event.
int cvedit_key(cvedit_t *e, const cvkey_t *k);

// The console's controller, once a frame: what went down (CVEDIT_UP...,
// cvin_pressed()) and the keypad key that went down or -1 (cvin_key()).
int cvedit_pad(cvedit_t *e, uint8_t pressed, int key);

// Every pass of the loop: a held key's repeats, and a save that was asked
// for once SAVING... has been drawn (saving a big file takes a second or two).
int cvedit_task(cvedit_t *e, uint32_t now_us);

// Bring the canvas up to date (all: draw everything, the canvas was cleared).
// Cheap when nothing changed. Only the cells that differ are touched, so a
// keystroke makes a short list.
void cvedit_draw(cvedit_t *e, canvas_t *c, bool all);

// The host drew over the region: draw it all next time.
void cvedit_redraw(cvedit_t *e);

// ---- files -----------------------------------------------------------------------------
// Look for a save cut short, in cfg.home (and dir, if not NULL). True if one
// was found: the editor is asking about it. cvedit_load() does this too.
bool cvedit_check(cvedit_t *e, const char *dir);

// Open path. f: the file as fsro has it, if the host knows (the launch
// block's, cvsd_launched_file(): a path with characters outside ASCII can't
// be looked up), or NULL. FS_OK or an FS_* error, which the bottom line
// explains. A file that isn't found or is too big leaves the text in the
// editor as it was; one that stops reading leaves a new text. If a save cut short
// is found first, the editor asks about that, and opens the file after if
// the answer is to drop it.
int cvedit_load(cvedit_t *e, const char *path, const fs_file_t *f);

// Save now (keypad 1 does it a moment later, after SAVING... is shown).
// CVEDIT_SAVED, CVEDIT_SAVE_FAILED, or CVEDIT_NONE if the text has no name
// yet: the editor is asking for one.
int cvedit_save(cvedit_t *e);

// An empty text with no name, whatever was there.
void cvedit_new(cvedit_t *e);

// ---- for the host ------------------------------------------------------------------------
// The cursor to the start of the file's line n (from 1; past the end: the
// last line), shown a third of the way down.
void cvedit_goto(cvedit_t *e, uint32_t line);

// The line the cursor is on, from 1.
uint32_t cvedit_line(const cvedit_t *e);

// The top line (NULL: the name, the * and the line number) and the bottom
// line (NULL: cfg.keys). The strings are kept, not copied.
void cvedit_bars(cvedit_t *e, const char *top, const char *bottom);

// A message on the bottom line until the next key ("ERROR AT LINE 12"); bad
// shows it as an error. Copied, cut to the line.
void cvedit_message(cvedit_t *e, const char *msg, bool bad);

// The text is e->cfg.text, e->len bytes (no 0 ends it). Has it changes that
// aren't saved; is the editor in the text (not in a list or a question, when
// host keys aren't reported); the file's path into buf ("" with no file).
bool cvedit_modified(const cvedit_t *e);
bool cvedit_editing(const cvedit_t *e);
void cvedit_path(const cvedit_t *e, char *buf, size_t max);

// Put text in the editor as if typed at the cursor (a host's paste, a test).
// False if it didn't all fit.
bool cvedit_insert(cvedit_t *e, const uint8_t *s, uint32_t n);

#ifdef CVEDIT_TEST
bool cvedit_test_starts_ok(cvedit_t *e);
#endif

#ifdef __cplusplus
}
#endif
