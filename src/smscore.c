// smscore.c: see smscore.h.
#include "smscore.h"

#include <string.h>

#include "shared.h"

smscv_t smscore_cv;
void (*smscore_line_hook)(int line, const uint8_t *buf);

// ---- SMS Plus's calls out (render.c, system.c) ------------------------------------------

void sms_render_line(int line, const uint8_t *buf) {
    if (smscore_line_hook) smscore_line_hook(line, buf);
    smscv_line(&smscore_cv, line, buf);
}

static void palette(int i) { smscv_palette(&smscore_cv, i, vdp.cram, cart.type, IS_TMS_MODE); }
void sms_palette_sync(int i) { palette(i); }
void sms_palette_syncGG(int i) { palette(i); }
void sms_palette_syncSG(int i) { palette(i); }

// (system_reset() calls it: the caller puts the .sav in after the start)
void system_load_sram(void) {}

static void log_byte(int b) { smscv_log(&smscore_cv, b); }

// ---- the calls in -------------------------------------------------------------------------

int smscore_type_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return SMSCV_SMS;
    char e[4] = {0};
    for (int i = 0; i < 3 && dot[1 + i]; i++) e[i] = (char)(dot[1 + i] | 0x20);
    if (!strcmp(e, "gg")) return SMSCV_GG;
    if (!strcmp(e, "sg") || !strcmp(e, "sc")) return SMSCV_SG;
    return SMSCV_SMS;
}

void smscore_start(const uint8_t *rom, uint32_t size, int type) {
    smscv_init(&smscore_cv);
    render_bg_off = 0;
    load_rom((uintptr_t)rom, (int)size, type == SMSCV_GG ? TYPE_GG : type == SMSCV_SG ? TYPE_SG : TYPE_SMS);
    system_init(0);                          // (no sound of its own: smscv takes the writes)
    snd.log = 1;
    snd.callback = log_byte;
    system_reset();
}

int smscore_type(void) { return IS_GG ? SMSCV_GG : IS_SG ? SMSCV_SG : SMSCV_SMS; }

void smscore_input(uint8_t pad0, uint8_t pad1, bool pause) {
    const uint8_t in[2] = {pad0, pad1};
    for (int n = 0; n < 2; n++) {
        int p = 0;
        if (in[n] & SMSCORE_UP) p |= INPUT_UP;
        if (in[n] & SMSCORE_DOWN) p |= INPUT_DOWN;
        if (in[n] & SMSCORE_LEFT) p |= INPUT_LEFT;
        if (in[n] & SMSCORE_RIGHT) p |= INPUT_RIGHT;
        if (in[n] & SMSCORE_B1) p |= INPUT_BUTTON1;
        if (in[n] & SMSCORE_B2) p |= INPUT_BUTTON2;
        input.pad[n] = p;
    }
    input.system = pause ? (IS_GG ? INPUT_START : INPUT_PAUSE) : 0;
}

void smscore_frame(nescv_t *enc, uint64_t index) {
    smscv_frame_start(&smscore_cv, enc, smscore_type());
    sms_frame(0);                            // (every line drawn: the sprite collision flag needs it)
    smscv_frame_end(&smscore_cv, index, vdp.reg[7]);
}

int smscore_psg(uint8_t *out) { return smscv_psg(&smscore_cv, out); }

void smscore_snap(bool on) { render_snap = on ? ~7 : ~0; }

void smscore_bg(int mode) {
    render_bg_off = mode == SMSCORE_BG_OFF;
    smscore_cv.grey = mode == SMSCORE_BG_GREY;
}

uint8_t *smscore_sram(void) { return IS_SG ? NULL : sms.sram; }
bool smscore_sram_on(void) { return !IS_SG && sms.save; }

// the PSG's registers as the game left them
typedef struct {
    uint8_t latch, noise, att[4];
    uint16_t tone[3];
} psg_regs_t;

bool smscore_save_state(FIL *f) {
    if (!system_save_state(f)) return false;
    const smscv_t *s = &smscore_cv;
    psg_regs_t r = {s->latch, s->noise, {s->att[0], s->att[1], s->att[2], s->att[3]}, {s->tone[0], s->tone[1], s->tone[2]}};
    UINT n;
    return f_write(f, &r, sizeof r, &n) == FR_OK;
}

bool smscore_load_state(FIL *f) {
    if (!system_load_state(f)) return false;
    psg_regs_t r;
    UINT n;
    if (f_read(f, &r, sizeof r, &n) != FR_OK) return false;
    smscv_t *s = &smscore_cv;
    s->latch = r.latch & 7;
    s->noise = r.noise & 7;
    for (int k = 0; k < 4; k++) s->att[k] = r.att[k] & 15;
    for (int k = 0; k < 3; k++) s->tone[k] = r.tone[k] & 0x3FF;
    s->noise_written = true;                 // all of it to the console again
    for (int k = 0; k < 3; k++) s->sent_tone[k] = -1;
    for (int k = 0; k < 4; k++) s->sent_att[k] = -1;
    s->log_kind = 0;
    return true;
}

uint32_t smscore_layout(void) {
    uint32_t h = 2166136261u;
    const uint32_t sizes[] = {sizeof(t_vdp), sizeof(t_sms), RAMSIZEBYTES, sizeof(Z80_Regs), sizeof(int), 0x40,
                              sizeof(t_SN76496), sizeof(psg_regs_t), 1 /* this format */};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) h = (h ^ sizes[i]) * 16777619u;
    return h;
}

uint16_t smscore_pc(void) { return (uint16_t)z80_get_pc(); }

uint32_t smscore_banks(void) {
    return (uint32_t)sms.fcr[0] << 24 | (uint32_t)sms.fcr[1] << 16 | (uint32_t)sms.fcr[2] << 8 | sms.fcr[3];
}

uint32_t smscore_tiles_decoded(void) { return cacheMisses; }
