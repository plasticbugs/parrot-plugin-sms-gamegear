// sms -- Master System, Game Gear and SG-1000 games on the ColecoVision: pick
// a .sms, .gg or .sg on the card in the menu and press A. SMS Plus (from
// pico-smsplus) runs it on the cart, and smscv and nescv send each picture
// and the sound to the console as the Game Boy kernel's frame lists. RESET
// goes back to the menu.
//
//   core 1  serves the kernel and a list a TV frame (cvframes.c)
//   core 0  this loop: for each TV frame the console starts, one frame of
//           the game (its picture to the encoder when it can take one), its
//           sound into the next list; the saves when they're due; the pads
//
// The Master System's picture is 256 x 192, the ColecoVision's too, so it
// goes across at 1:1; a Game Gear's 160 x 144 sits in the middle. The PSG is
// the ColecoVision's own chip. See ../smscv.h.
//
// Memory: past .bss, up to the launch block, is free (no heap). SMS Plus's
// memory (77K) goes there, then the ROM if it fits; else the ROM goes in the
// PSRAM and is read through the XIP cache, which is the PSRAM's alone (this
// plugin runs from SRAM).
//
// The controller: joystick = D-pad, left fire = button 1, right fire =
// button 2, # = Pause (the Game Gear's Start). Keypad 0 switches the
// scrolling: smooth (the background moves as the game's does) or by tiles
// (on its 8-pixel grid: more pictures a second, as the console then holds
// most cells already). Keypad 1 switches the background: as the game's, grey
// (black and grey behind the sprites, which keep their colours), or off
// (the sprites over the backdrop's colour); see ../smscv.h. 7 held for a
// second saves a state, 9 loads it (the border turns yellow while it's
// held, then green, cyan or red).
//
// USB game pads (cvpad.h; a Switch Pro Controller or a plain HID pad,
// through a powered hub on the cart's USB port): the first is player 1,
// with the console's controller, the second player 2. D-pad or left stick
// = D-pad, the bottom face button = button 1 and the right one = button 2,
// + = Pause / Start; R switches the scrolling, L the background; ZL held for
// a second saves a state, ZR loads it.
//
// Saves, next to the ROM by its name: "<name>.sav", the cart's battery RAM
// (32K, as other emulators keep it), read when the game starts and written
// a sector at a time, only the sectors the game changed (a second after it
// stops writing to it, every ten seconds while it keeps on, and at RESET);
// and "<name>.state", the whole console (keypad 7 / 9). A card taken out and
// put back is written to again only if it holds the ROM where it was.
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/xip_cache.h"

#include "cvdiag.h"
#include "cvframes.h"
#include "cvpad.h"
#include "cvpsram.h"
#include "cvsd.h"
#include "font8.h"
#include "gbkernel.h"
#include "handoff.h"
#include "smscore.h"

#define OPEN_US        20000        // serve the console this long before the card's (blocking) start
#define WATCHDOG_MS    4000         // a hang reboots into the menu (with where the Z80 was)
#define NOTE_US        1000000      // the menu's note is brought up to date this often
#define CARD_SETTLE_US 500000       // a card put back: in this long before it's mounted
#define HOLD_FRAMES    60           // keypad 7 / 9 (ZL / ZR) held this long: a state saved / loaded

// TMS9918 colours
enum { C_BLACK = 1, C_DBLUE = 4, C_LBLUE = 5, C_CYAN = 7, C_MRED = 8, C_DYELLOW = 10, C_LYELLOW = 11,
       C_LGREEN = 3, C_GRAY = 14, C_WHITE = 15 };

extern uint8_t __bss_end__[];       // (the linker's) the free RAM: from here to the launch block
#ifndef SMS_FREE_END                // (test/sms_plugin_host.c has its own)
#define SMS_FREE_END ((uint8_t *)CVL_ADDR)
#endif

static nescv_t enc;
static uint8_t list[NESCV_LIST_MAX] __attribute__((aligned(4)));
static size_t list_len;
static bool list_pending;           // built, not published yet (cvf_publish said wait)
static uint32_t seen_frames;
static uint64_t pic_index;
static fs_t fs;
static fs_file_t file;
static char path[sizeof CVL_BLOCK->path];
static char rom_dir[sizeof CVL_BLOCK->path];  // the ROM's folder ("" the root)
static char name[33], note[33];
static int type = SMSCV_SMS;
static uint8_t *free_at;

static void feed(void) { if (watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS) watchdog_update(); }

static void __attribute__((noreturn)) fail(const char *why, const char *more, const char *menu_note);

static void *take(uint32_t n) {     // n bytes of the free RAM, or NULL
    uint8_t *p = (uint8_t *)(((uintptr_t)free_at + 3) & ~(uintptr_t)3);
    if (n > (uint32_t)(SMS_FREE_END - p)) return NULL;
    free_at = p + n;
    return p;
}

static uint32_t free_left(void) { return (uint32_t)(SMS_FREE_END - free_at); }

// SMS Plus's memory (system_init): taken for good
void *frens_f_malloc(size_t n) {
    void *p = take((uint32_t)n);
    if (!p) fail("NO ROOM FOR THE CONSOLE", NULL, "SMS: no room for SMS Plus");
    memset(p, 0, n);
    return p;
}

void frens_f_free(void *p) { (void)p; }   // (system_shutdown(), which never comes)

// ---- the console, a list a TV frame ------------------------------------------------

static void publish(void) {
    if (list_pending && cvf_publish(list, list_len)) list_pending = false;
}

// a new TV frame started since the last call (and the last list is out)
static bool new_frame(void) {
    cvpad_task();
    cvf_task(time_us_32());
    feed();
    publish();
    if (list_pending) return false;
    uint32_t f = cvf_frames();
    if (f == seen_frames) return false;
    seen_frames = f;
    return true;
}

static void send(const uint8_t *psg, int npsg) {
    list_len = nescv_tv_frame(&enc, psg, npsg, list);
    list_pending = true;
    publish();
}

// ---- messages: a picture of text, a band at a time -----------------------------------

typedef struct {
    int16_t x, y;
    uint8_t colour, scale;
    char s[33];
} text_t;

static text_t texts[8];
static int ntexts;
static int bar = -1;                // loading: a bar this many percent full (-1: none)
static bool msg_waiting;
static uint8_t msg_lut[256];

static void add_text(int y, const char *s, uint8_t colour, int scale) {
    if (!s || ntexts == 8) return;
    text_t *t = &texts[ntexts++];
    snprintf(t->s, sizeof t->s, "%s", s);
    int w = (int)strlen(t->s) * 8 * scale;
    t->x = (int16_t)(w < SMSCV_W ? (SMSCV_W - w) / 2 : 0);
    t->y = (int16_t)y;
    t->colour = colour;
    t->scale = (uint8_t)scale;
}

static void fill(uint8_t band[8][SMSCV_W], int r, int x, int y, int w, int h, uint8_t colour) {
    for (int k = 0; k < 8; k++) {
        int py = r * 8 + k;
        if (py >= y && py < y + h) memset(&band[k][x], colour, (size_t)w);
    }
}

static void offer_msg(void) {
    if (!nescv_wants_picture(&enc)) { msg_waiting = true; return; }
    static uint8_t band[8][SMSCV_W];
    for (int r = 0; r < NESCV_ROWS; r++) {
        memset(band, C_BLACK, sizeof band);
        for (int i = 0; i < ntexts; i++) {
            const text_t *t = &texts[i];
            for (int k = 0; k < 8; k++) {
                int gy = r * 8 + k - t->y;
                if (gy < 0 || gy >= 8 * t->scale) continue;
                int x = t->x;
                for (const char *c = t->s; *c; c++, x += 8 * t->scale) {
                    int ch = (unsigned char)*c;
                    if (ch >= 'a' && ch <= 'z') ch -= 32;
                    if (ch < 32 || ch > 95) ch = '?';
                    uint8_t g = font8[ch - 32][gy / t->scale];
                    for (int b = 0; b < 8 * t->scale && x + b < SMSCV_W; b++)
                        if (g >> (7 - b / t->scale) & 1) band[k][x + b] = t->colour;
                }
            }
        }
        if (bar >= 0) {             // loading (the font has no %)
            fill(band, r, 76, 128, 104, 8, C_GRAY);
            fill(band, r, 77, 129, 102, 6, C_BLACK);
            if (bar) fill(band, r, 78, 130, bar, 4, C_WHITE);
        }
        nescv_band(&enc, r, &band[0][0], SMSCV_W, msg_lut);
    }
    nescv_band_done(&enc, ++pic_index, C_BLACK);
    msg_waiting = false;
}

static const char *system_name(void) {
    return type == SMSCV_GG ? "GAME GEAR" : type == SMSCV_SG ? "SG-1000" : "MASTER SYSTEM";
}

static void message(const char *head, const char *l1, const char *l2) {
    ntexts = 0;
    add_text(48, system_name(), C_LBLUE, 2);
    if (name[0]) add_text(88, name, C_WHITE, 1);
    add_text(112, head, C_LYELLOW, 1);
    if (bar < 0) {
        add_text(128, l1, C_GRAY, 1);
        add_text(140, l2, C_GRAY, 1);
    }
    add_text(176, "RESET: BACK TO THE MENU", C_DBLUE, 1);
    offer_msg();
}

// serve the console, a list a frame, for us microseconds (forever: 0)
static void serve_for(uint32_t us) {
    uint32_t t0 = time_us_32();
    while (!us || time_us_32() - t0 < us) {
        if (!new_frame()) continue;
        if (msg_waiting) offer_msg();
        send(NULL, 0);
    }
}

static void __attribute__((noreturn)) fail(const char *why, const char *more, const char *menu_note) {
    bar = -1;
    message("IT CAN'T RUN", why, more);
    if (menu_note) cvf_note(menu_note);
    serve_for(0);
    for (;;) tight_loop_contents();
}

static const char *fs_why(int r) {
    switch (r) {
    case FS_EIO:     return cvsd_card_in() ? "THE CARD DIDN'T ANSWER" : "NO CARD IN THE SLOT";
    case FS_ENOFS:   return "NO FAT OR EXFAT ON THE CARD";
    case FS_ENOFILE: return "THE FILE IS EMPTY OR GONE";
    case FS_ECHAIN:  return "ITS CLUSTERS ARE BROKEN";
    case FS_EFRAG:   return "IT'S IN TOO MANY PIECES";
    default:         return "THE CARD'S FILE SYSTEM IS ODD";
    }
}

// ---- the ROM ------------------------------------------------------------------------

static uint32_t rom_sum;            // FNV-1a of the file (a state is this ROM's)
static bool rom_in_psram;

// the file into SRAM (dst) or the PSRAM (dst NULL), a sector between TV frames
static void read_rom(uint8_t *dst) {
    static uint32_t sec[128];
    uint32_t n = (uint32_t)file.size, last_pc = 101, h = 2166136261u;
    for (uint32_t at = 0; at < n; at += 512) {
        uint32_t lba = fs_lba(&fs, at / 512);
        if (!lba || !cvsd_read(lba, (uint8_t *)sec)) fail("A CARD READ FAILED", NULL, "SMS: a card read failed");
        const uint8_t *b = (const uint8_t *)sec;
        for (uint32_t i = 0; i < 512 && at + i < n; i++) h = (h ^ b[i]) * 16777619u;
        if (dst) memcpy(dst + at, sec, 512);   // (the buffer runs 16K past the file)
        else {
            volatile uint32_t *d = CVPSRAM_UNCACHED + at / 4;
            for (int i = 0; i < 128; i++) d[i] = sec[i];
        }
        uint32_t pc = (uint32_t)((uint64_t)at * 100 / n);
        if (pc / 5 != last_pc / 5) {
            last_pc = pc;
            bar = (int)pc;
            message("LOADING", NULL, NULL);
        }
        if (new_frame()) {
            if (msg_waiting) offer_msg();
            send(NULL, 0);
        }
    }
    if (!dst) xip_cache_invalidate_all();
    bar = -1;
    rom_sum = h;
}

// ---- saves --------------------------------------------------------------------------
#define SAV_QUIET_US   1000000      // no changes for this long: written
#define SAV_EVERY_US   10000000     // ...or changing for this long
#define SAV_RETRY_US   5000000      // after a write failed
#define SAV_LOOK       30           // the battery RAM is looked at for changes every this many frames
#define SAV_SECS       (SMSCORE_SRAM / 512)
#define ST_FILE        0x10000      // the .state's size: a header sector, then the state
#define ST_MAGIC       "CVSMSST1"

static uint16_t rom_nm[FS_LNAME], sav_nm[FS_LNAME], st_nm[FS_LNAME];
static int rom_nm_n, sav_n, st_n;   // 0: no name (unreadable, or too long)
static fs_file_t sav_file;
static uint32_t card_gen = 1;       // cards mounted so far (the .sav is looked for again on a new one)
static uint32_t sav_gen;            // card_gen sav_file is from (0: none)
static uint32_t sav_hash[SAV_SECS]; // each sector as last written (or read)
static uint64_t sav_pending;        // sectors changed, not written yet
static uint32_t sav_change_t, sav_dirty_t, sav_retry_t;
static uint32_t sav_writes, sav_fails, st_saves, st_loads, st_fails;
static char sav_msg[80] = "";       // what the saves last did: 25 characters (the menu shows 30 of a note)
static uint8_t sav_buf[512] __attribute__((aligned(4)));

// the card: the one the game came from, mounted (card_watch)
static bool card_here = true, card_tried;
static uint32_t card_in_t;

static uint32_t sec_hash(const uint8_t *p) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 512; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int with_ext(uint16_t *out, const uint16_t *nm, int stem, const char *ext) {
    size_t e = strlen(ext);
    if (stem + (int)e > 255) return 0;
    memcpy(out, nm, (size_t)stem * 2);
    for (size_t i = 0; i < e; i++) out[stem + (int)i] = (uint16_t)ext[i];
    return stem + (int)e;
}

// the names, and the .sav into the battery RAM (after the start)
static void saves_start(void) {
    uint8_t *sram = smscore_sram();
    for (int k = 0; sram && k < SAV_SECS; k++) sav_hash[k] = sec_hash(sram + k * 512);
    int n = rom_nm_n = fs_name16(&fs, rom_dir, file.first_clus, rom_nm, FS_LNAME - 1);
    if (n <= 0) { rom_nm_n = 0; snprintf(sav_msg, sizeof sav_msg, "no saves: bad name (%d)", n); return; }
    int stem = n;
    for (int i = 0; i < n; i++) if (rom_nm[i] == '.') stem = i;
    sav_n = with_ext(sav_nm, rom_nm, stem, ".sav");
    st_n = with_ext(st_nm, rom_nm, stem, ".state");
    if (!sram) { sav_n = 0; snprintf(sav_msg, sizeof sav_msg, "SG-1000: no battery save"); return; }
    fs_file_t f;
    bool made;
    int r = fs_create(&fs, rom_dir, sav_nm, sav_n, 0, &f, &made);
    if (r == FS_ENOFILE) { snprintf(sav_msg, sizeof sav_msg, "no .sav yet"); return; }
    if (r || f.size < 512 || f.size > 0x40000) {   // never written over: it isn't this game's
        snprintf(sav_msg, sizeof sav_msg, r ? ".sav unreadable (%d)" : ".sav odd size: not used", r);
        sav_n = 0;
        return;
    }
    if (fs_open(&fs, &f)) { snprintf(sav_msg, sizeof sav_msg, ".sav won't open: not used"); sav_n = 0; return; }
    uint32_t n_sec = (uint32_t)(f.size < SMSCORE_SRAM ? f.size : SMSCORE_SRAM) / 512;
    for (uint32_t k = 0; k < n_sec; k++) {
        uint32_t lba = fs_lba(&fs, k);
        if (!lba || !cvsd_read(lba, sram + k * 512)) {
            memset(sram, 0, SMSCORE_SRAM);
            snprintf(sav_msg, sizeof sav_msg, ".sav didn't read: unused");
            sav_n = 0;
            return;
        }
        sav_hash[k] = sec_hash(sram + k * 512);
    }
    sav_file = f;
    sav_gen = card_gen;
    snprintf(sav_msg, sizeof sav_msg, "%lu-byte .sav loaded", (unsigned long)f.size);
}

// the .sav, opened for writing: found again on a new card, or made
static bool sav_open(void) {
    if (sav_gen != card_gen) {
        fs_file_t f;
        bool made;
        int r = fs_create(&fs, rom_dir, sav_nm, sav_n, SMSCORE_SRAM, &f, &made);
        if (r || f.size < SMSCORE_SRAM) {
            snprintf(sav_msg, sizeof sav_msg, r ? ".sav can't be made (%d)" : ".sav on card too small", r);
            if (!r) sav_n = 0;      // someone else's: left alone
            return false;
        }
        if (made) {                 // all of it: the card held anything there
            sav_pending = ~0ull;
            snprintf(sav_msg, sizeof sav_msg, ".sav made");
        }
        sav_file = f;
        sav_gen = card_gen;
    }
    return fs_open(&fs, &sav_file) == FS_OK;
}

// the sectors the game changed since they were written, into sav_pending
static void sav_look(void) {
    const uint8_t *sram = smscore_sram();
    if (!sram) return;
    for (int k = 0; k < SAV_SECS; k++) {
        uint32_t h = sec_hash(sram + k * 512);
        if (h != sav_hash[k]) {
            sav_hash[k] = h;
            sav_pending |= 1ull << k;
        }
    }
}

// write what changed; true when nothing is left
static bool sav_flush(void) {
    if (!sav_n || !smscore_sram_on()) return true;
    sav_look();
    if (!sav_pending) return true;
    if (!card_here) return false;
    if (!sav_open()) { sav_fails++; return false; }
    const uint8_t *sram = smscore_sram();
    for (int k = 0; k < SAV_SECS; k++) {
        if (!(sav_pending >> k & 1)) continue;
        memcpy(sav_buf, sram + k * 512, 512);
        if (fs_write_sec(&fs, (uint32_t)k, sav_buf)) { sav_fails++; snprintf(sav_msg, sizeof sav_msg, "the .sav didn't write"); return false; }
        sav_pending &= ~(1ull << k);
        feed();
    }
    sav_writes++;
    return true;
}

// each TV frame while the game runs: the .sav when it's due
static void saves_service(uint32_t frames, uint32_t now) {
    if (!sav_n || !smscore_sram_on()) return;
    if (frames % SAV_LOOK == 0) {
        uint64_t before = sav_pending;
        sav_look();
        if (sav_pending != before) {
            sav_change_t = now;
            if (!sav_dirty_t) sav_dirty_t = now | 1;
        }
    }
    bool due = sav_dirty_t && (now - sav_change_t > SAV_QUIET_US || now - sav_dirty_t > SAV_EVERY_US);
    if (!due || (sav_retry_t && (int32_t)(now - sav_retry_t) < 0)) return;
    if (sav_flush()) sav_dirty_t = sav_retry_t = 0;
    else sav_retry_t = (now + SAV_RETRY_US) | 1;
}

// ---- states: SMS Plus's, streamed to and from the .state's sectors ------------------------

typedef struct {
    char magic[8];
    uint32_t layout;                // smscore_layout(): this firmware's
    uint32_t rom_size, rom_sum;     // this ROM's
    uint32_t len, sum;              // the state's bytes after this sector, and their FNV-1a
} st_head_t;

typedef struct {
    uint32_t sec;                   // the file's next sector
    uint32_t at;                    // bytes of sav_buf used (writing) or taken (reading)
    uint32_t len, sum;
    bool dry;                       // writing: only count and sum
    bool bad;
} st_io_t;

static UINT st_io(FIL *f, void *buf, UINT n, int write) {
    st_io_t *s = f->ctx;
    uint8_t *p = buf;
    UINT done = 0;
    while (done < n && !s->bad) {
        if (!write && s->at == 512) {
            uint32_t lba = fs_lba(&fs, s->sec++);
            if (!lba || !cvsd_read(lba, sav_buf)) { s->bad = true; break; }
            s->at = 0;
            feed();
        }
        UINT k = n - done < 512 - s->at ? n - done : 512 - s->at;
        if (write) memcpy(sav_buf + s->at, p + done, k);
        else memcpy(p + done, sav_buf + s->at, k);
        for (UINT i = 0; i < k; i++) s->sum = (s->sum ^ p[done + i]) * 16777619u;
        s->at += k;
        s->len += k;
        done += k;
        if (write && s->at == 512) {
            if (!s->dry && fs_write_sec(&fs, s->sec, sav_buf)) { s->bad = true; break; }
            s->sec++;
            s->at = 0;
            feed();
        }
    }
    return done;
}

static bool state_file(bool make, fs_file_t *f, bool *made) {
    if (!st_n) { snprintf(sav_msg, sizeof sav_msg, "no states: bad ROM name"); return false; }
    if (!card_here) { snprintf(sav_msg, sizeof sav_msg, "no states: card is out"); return false; }
    int r = fs_create(&fs, rom_dir, st_nm, st_n, make ? ST_FILE : 0, f, made);
    if (r) { snprintf(sav_msg, sizeof sav_msg, r == FS_ENOFILE ? "no .state to load" : ".state can't be %s (%d)", make ? "made" : "found", r); return false; }
    if (f->size < ST_FILE) { snprintf(sav_msg, sizeof sav_msg, ".state on card too small"); return false; }
    return fs_open(&fs, f) == FS_OK;
}

static bool state_save(void) {
    fs_file_t f;
    bool made;
    if (!state_file(true, &f, &made)) return false;
    uint32_t lba;
    if (!made) {                    // someone else's file of that name: left alone
        if (!(lba = fs_lba(&fs, 0)) || !cvsd_read(lba, sav_buf)) return false;
        if (memcmp(sav_buf, ST_MAGIC, 8)) { snprintf(sav_msg, sizeof sav_msg, ".state not ours: kept"); return false; }
    }
    st_io_t io = {.sec = 1, .sum = 2166136261u, .dry = true};
    FIL fil = {st_io, &io};
    if (!smscore_save_state(&fil) || io.len > ST_FILE - 512) { snprintf(sav_msg, sizeof sav_msg, "the state is too big"); return false; }
    st_head_t h = {.layout = smscore_layout(), .rom_size = (uint32_t)file.size, .rom_sum = rom_sum, .len = io.len, .sum = io.sum};
    memcpy(h.magic, ST_MAGIC, 8);
    memset(sav_buf, 0, 512);
    memcpy(sav_buf, &h, sizeof h);
    if (fs_write_sec(&fs, 0, sav_buf)) { snprintf(sav_msg, sizeof sav_msg, "the .state didn't write"); return false; }
    io = (st_io_t){.sec = 1, .sum = 2166136261u};
    if (!smscore_save_state(&fil) || io.bad) { snprintf(sav_msg, sizeof sav_msg, "the .state didn't write"); return false; }
    if (io.at) {                    // the last sector, partly
        memset(sav_buf + io.at, 0, 512 - io.at);
        if (fs_write_sec(&fs, io.sec, sav_buf)) { snprintf(sav_msg, sizeof sav_msg, "the .state didn't write"); return false; }
    }
    snprintf(sav_msg, sizeof sav_msg, "state saved (%lu bytes)", (unsigned long)io.len);
    return true;
}

static bool state_load(void) {
    fs_file_t f;
    bool made;
    if (!state_file(false, &f, &made)) return false;
    uint32_t lba;
    if (!(lba = fs_lba(&fs, 0)) || !cvsd_read(lba, sav_buf)) { snprintf(sav_msg, sizeof sav_msg, "the .state didn't read"); return false; }
    st_head_t h;
    memcpy(&h, sav_buf, sizeof h);
    if (memcmp(h.magic, ST_MAGIC, 8)) { snprintf(sav_msg, sizeof sav_msg, "the .state isn't a state"); return false; }
    if (h.layout != smscore_layout()) { snprintf(sav_msg, sizeof sav_msg, ".state from another build"); return false; }
    if (h.rom_size != file.size || h.rom_sum != rom_sum) { snprintf(sav_msg, sizeof sav_msg, ".state is another game's"); return false; }
    if (h.len > ST_FILE - 512) { snprintf(sav_msg, sizeof sav_msg, "the .state is damaged"); return false; }
    // all of it, checked, before any goes in
    st_io_t io = {.sec = 1, .at = 512, .sum = 2166136261u};
    FIL fil = {st_io, &io};
    static uint8_t chunk[256];
    for (uint32_t left = h.len; left && !io.bad;) {
        UINT k = left < sizeof chunk ? left : sizeof chunk;
        st_io(&fil, chunk, k, 0);
        left -= k;
    }
    if (io.bad || io.sum != h.sum) { snprintf(sav_msg, sizeof sav_msg, io.bad ? "the .state didn't read" : "the .state is damaged"); return false; }
    io = (st_io_t){.sec = 1, .at = 512, .sum = 2166136261u};
    bool ok = smscore_load_state(&fil);
    if (!ok || io.bad || io.sum != h.sum) {
        snprintf(sav_msg, sizeof sav_msg, "state half loaded: RESET");
        return false;
    }
    snprintf(sav_msg, sizeof sav_msg, "state loaded");
    return true;
}

// the card out and back: mounted again, and written to only if it's the one
// the game came from (the ROM's name at its first cluster in its folder)
static void card_watch(uint32_t now) {
    if (!cvsd_card_in()) {
        card_here = card_tried = false;
        card_in_t = 0;
        return;
    }
    if (card_here || card_tried) return;
    if (!card_in_t) { card_in_t = now | 1; return; }
    if (now - card_in_t < CARD_SETTLE_US) return;
    card_tried = true;              // once a time it goes in
    if (!cvsd_init() || fs_mount(&fs, cvsd_read) != FS_OK) { snprintf(sav_msg, sizeof sav_msg, "card back: didn't mount"); return; }
    fs_set_writer(&fs, cvsd_write);
    static uint16_t nm[FS_LNAME];
    int n = rom_nm_n ? fs_name16(&fs, rom_dir, file.first_clus, nm, FS_LNAME - 1) : -1;
    if (n != rom_nm_n || memcmp(nm, rom_nm, (size_t)n * 2)) {
        snprintf(sav_msg, sizeof sav_msg, "not the game's card");
        return;
    }
    card_here = true;
    card_gen++;
    snprintf(sav_msg, sizeof sav_msg, "the game's card is back");
}

// ---- the game -----------------------------------------------------------------------

static bool playing, snapped;
static int bg_mode;                 // SMSCORE_BG_ALL, _GREY, _OFF
static uint8_t last_key = 0x0F;
static uint32_t last_usb;           // pad 0's buttons, for the presses
static uint32_t late, us_max, shown_at_note;
static uint8_t hold_key;            // 0x5 (7) or 0xB (9) held, for hold_n frames
static uint32_t hold_n;
static int req;                     // a state request (0x5 save, 0xB load), done at req_frame
static uint32_t req_frame, flash_until;

static uint8_t pad_from_cv(uint8_t joy, uint8_t fire) {
    uint8_t p = 0;
    if (!(joy & 1)) p |= SMSCORE_UP;
    if (!(joy & 2)) p |= SMSCORE_RIGHT;
    if (!(joy & 4)) p |= SMSCORE_DOWN;
    if (!(joy & 8)) p |= SMSCORE_LEFT;
    if (!(fire & 1)) p |= SMSCORE_B1;
    if (!(fire & 2)) p |= SMSCORE_B2;
    return p;
}

static uint8_t pad_from_usb(const gamepad_t *g, bool *pause) {
    const uint32_t b = g->buttons, d = cvpad_dirs(g);
    uint8_t p = 0;
    if (d & GP_UP) p |= SMSCORE_UP;
    if (d & GP_DOWN) p |= SMSCORE_DOWN;
    if (d & GP_LEFT) p |= SMSCORE_LEFT;
    if (d & GP_RIGHT) p |= SMSCORE_RIGHT;
    if (b & GP_SOUTH) p |= SMSCORE_B1;
    if (b & GP_EAST) p |= SMSCORE_B2;
    if (b & GP_PLUS) *pause = true;
    return p;
}

static void snap(bool on) {
    snapped = on;
    smscore_snap(on);
}

// keypad 1 (or L): the next background (not *: Select in the NES plugin,
// so * and # stay the games' own keys in every plugin)
static void next_bg(void) {
    bg_mode = (bg_mode + 1) % SMSCORE_BG_N;
    smscore_bg(bg_mode);
}

// keypad 7 / 9 (or ZL / ZR) held: the border yellow; at a second, the
// request, done once the console shows the border (two TV frames on), the
// picture standing still meanwhile
static void holds(uint8_t key, uint32_t frames) {
    if (req) return;
    if (key == 0x5 || key == 0xB) {
        if (key != hold_key) { hold_key = key; hold_n = 0; }
        if (++hold_n == HOLD_FRAMES) {
            req = key;
            req_frame = frames + 2;
        }
        if (hold_n <= HOLD_FRAMES) smscore_cv.border = C_DYELLOW;
        return;
    }
    hold_key = 0;
    hold_n = 0;
    if ((int32_t)(frames - flash_until) >= 0) smscore_cv.border = 0;
}

static void requests(uint32_t frames) {
    if (!req || (int32_t)(frames - req_frame) < 0) return;
    bool ok = req == 0x5 ? state_save() : state_load();
    if (req == 0x5) ok ? st_saves++ : st_fails++;
    else ok ? st_loads++ : st_fails++;
    smscore_cv.border = ok ? (req == 0x5 ? C_LGREEN : C_CYAN) : C_MRED;
    flash_until = frames + (ok ? 45 : 120);
    snprintf(note, sizeof note, "SMS: %.25s", sav_msg);
    cvf_note(note);
    req = 0;
}

static void game_frame(uint32_t frames) {
    uint32_t t0 = time_us_32();
    uint8_t joy, fire, key;
    cvf_input(&joy, &fire, &key);
    if (key != last_key && key == 0xA) snap(!snapped);   // 0, on the press
    if (key != last_key && key == 0xD) next_bg();        // 1
    last_key = key;
    uint8_t p0 = pad_from_cv(joy, fire), p1 = 0;
    bool pause = key == 0x6;        // #
    gamepad_t g;
    if (cvpad_get(0, &g)) {
        p0 |= pad_from_usb(&g, &pause);
        const uint32_t down = g.buttons & ~last_usb;
        if (down & GP_R) snap(!snapped);
        if (down & GP_L) next_bg();
        if (key == 0xF && (g.buttons & GP_ZL)) key = 0x5;   // as keypad 7 / 9 held
        else if (key == 0xF && (g.buttons & GP_ZR)) key = 0xB;
        last_usb = g.buttons;
    } else last_usb = 0;
    if (cvpad_get(1, &g)) p1 = pad_from_usb(&g, &pause);
    // both of a pair at once (the console's stick and a pad): neither, as on a real pad
    if ((p0 & (SMSCORE_UP | SMSCORE_DOWN)) == (SMSCORE_UP | SMSCORE_DOWN)) p0 &= (uint8_t)~(SMSCORE_UP | SMSCORE_DOWN);
    if ((p0 & (SMSCORE_LEFT | SMSCORE_RIGHT)) == (SMSCORE_LEFT | SMSCORE_RIGHT)) p0 &= (uint8_t)~(SMSCORE_LEFT | SMSCORE_RIGHT);
    holds(key, frames);
    smscore_input(p0, p1, pause);
    smscore_frame(nescv_wants_picture(&enc) ? &enc : NULL, ++pic_index);
    uint8_t psg[SMSCV_PSG_MAX];
    send(psg, smscore_psg(psg));
    uint32_t us = time_us_32() - t0;
    if (us > us_max) us_max = us;
}

// on the way back to the menu (cvf_on_exit): the game's last changes
static void going_back(void) {
    if (!playing) return;
    bool ok = sav_flush();
    if (!ok || sav_fails || st_fails) {
        char s[33];
        snprintf(s, sizeof s, "SMS: %.25s", !ok && !card_here ? "card out: .sav not saved" : sav_msg);
        cvf_note(s);
    }
}

// the menu's note, should RESET come: the scrolling (sm, ti) and the
// background (A all, G grey, O off), pictures this second, TV frames the
// cart was late for, its slowest frame, then the RAM left ("m"), or once a
// pad has been in, the longest cvpad_task() in ms; 30 characters at most,
// what the menu shows. And in the return block now, where the Z80 is,
// should the watchdog reboot into the menu.
static uint32_t mem_left;

static void update_note(void) {
    static uint32_t t_last, late_last;
    uint32_t t = time_us_32();
    if (t - t_last < NOTE_US) return;
    snprintf(note, sizeof note, "SMS stuck: pc %04X b %08lX", smscore_pc(), (unsigned long)smscore_banks());
    cvf_note(note);
    cvf_note_now();
    uint32_t shown = enc.stats.shown + enc.stats.same;
    unsigned per_s = MIN(shown - shown_at_note, 99u), dl = MIN(late - late_last, 99u),
             ms = MIN(us_max / 1000, 99u), tenths = us_max / 100 % 10;
    int n = snprintf(note, sizeof note, "SMS %s%c %u %uL %u.%ums ", snapped ? "ti" : "sm", "AGO"[bg_mode], per_s, dl, ms, tenths);
    if (cvpad_seen()) {
        unsigned pad = MIN(cvpad_task_max_us() / 10, 999u);
        snprintf(note + n, sizeof note - (size_t)n, "pad %u.%02u", pad / 100, pad % 100);
    } else {
        snprintf(note + n, sizeof note - (size_t)n, "m %lu", (unsigned long)mem_left);
    }
    cvf_note(note);
    t_last = t;
    late_last = late;
    shown_at_note = shown;
    us_max = 0;
}

int main(void) {
    static const cvf_config_t cfg = {gbkernel_bin, GBKERNEL_BIN_LEN, handoff_bin, HANDOFF_BIN_LEN};
    cvf_start(&cfg);
    cvd_start();                    // a fault, a hang or a panic: what, in the menu's note (cvdiag.h)
    cvd_stage(1);
    cvpad_init();                   // the pads find their way in while the game loads
    nescv_init(&enc);
    for (int i = 0; i < 256; i++) msg_lut[i] = (i & 15) ? (uint8_t)(i & 15) : (uint8_t)C_BLACK;
    free_at = __bss_end__;

    const cvl_t *b = cvf_launch();
    if (b && b->path[0]) {          // (the block becomes a return block below: what's needed, copied)
        memcpy(path, b->path, sizeof path);
        const char *slash = strrchr(path, '/');
        const char *file = slash ? slash + 1 : path;
        // the system from the whole name: most Game Gear games' names are
        // longer than the title's line, and a cut one loses its ".gg"
        type = smscore_type_of(file);
        // the title: the name without its extension, a line of the screen at most
        const char *dot = strrchr(file, '.');
        snprintf(name, sizeof name, "%.*s", dot && dot != file ? (int)(dot - file) : (int)strlen(file), file);
        size_t d = slash ? (size_t)(slash - path) : 0;
        memcpy(rom_dir, path, d);
        rom_dir[d] = 0;
    }
    message(path[0] ? "STARTING" : NULL, path[0] ? NULL : "OPEN A .SMS, .GG OR .SG FILE", path[0] ? NULL : "FROM THE MENU");
    serve_for(OPEN_US);
    if (!path[0]) fail("OPEN A .SMS, .GG OR .SG FILE", "FROM THE MENU", NULL);

    int r = cvsd_open_launched(&fs, b, true, &file);   // (blocks up to 1.5 s: the last list runs again meanwhile)
    if (r != FS_OK) fail(fs_why(r), NULL, "SMS: the file couldn't be read");
    cvd_stage(2);
    if (file.size < 0x400 || file.size > CVPSRAM_SIZE - 0x10000) fail("IT ISN'T A GAME'S ROM", "(TOO SMALL OR TOO BIG)", "SMS: not a ROM");
    // SMS Plus's memory first (what system_init takes), then the ROM if it fits
    const uint32_t core = 512 * 4 * 2 + CACHEDTILES * 64 + 0x2000 + 0x8000 + 0x400;
    const uint32_t alloc = (uint32_t)file.size + 0x4000;   // (a partial last bank is read whole)
    uint8_t *sram_rom = free_left() >= core + alloc ? take(alloc) : NULL;
    if (!sram_rom && !cvpsram_init()) fail("THE PSRAM DIDN'T ANSWER", "(THE ROM NEEDS IT)", "SMS: the PSRAM didn't answer");
    watchdog_enable(WATCHDOG_MS, true);
    rom_in_psram = !sram_rom;
    cvd_stage(3);
    read_rom(sram_rom);
    cvd_stage(4);

    message("STARTING", NULL, NULL);
    smscore_start(sram_rom ? sram_rom : (const uint8_t *)CVPSRAM_CACHED, (uint32_t)file.size, type);
    cvd_stage(5);
    mem_left = free_left();
    saves_start();
    cvd_stage(6);
    snprintf(note, sizeof note, "SMS: %.25s", sav_msg);
    cvf_note(note);
    cvf_on_exit(going_back);
    playing = true;

    for (;;) {
        uint32_t before = seen_frames;
        if (!new_frame()) continue;
        if (before && seen_frames - before > 1) late += seen_frames - before - 1;
        game_frame(seen_frames);
        uint32_t now = time_us_32();
        requests(seen_frames);
        saves_service(seen_frames, now);
        card_watch(now);
        update_note();
    }
}
