// smscv.c: see smscv.h.
#include "smscv.h"

#include <string.h>

#define C_BLACK 1
#define C_GREY 14

// Nearest in OKLab with lightness differences weighted 0.4 (hue counts for
// more: Fantasy Zone's pink clouds go to magenta, not grey; pure green to
// medium green); the neutral ones (R = G = B) to black, grey or white only:
// 85 grey goes to black, 170 to grey (14), 255 to white.
const uint8_t smscv_sms_colours[64] = {
    1, 1, 6, 8, 12, 12, 6, 8, 12, 12, 10, 10, 2, 2, 3, 10,
    1, 13, 6, 8, 12, 1, 6, 8, 12, 12, 10, 11, 3, 3, 3, 11,
    4, 4, 13, 13, 4, 4, 13, 13, 7, 7, 14, 9, 3, 3, 3, 11,
    4, 4, 5, 13, 4, 4, 5, 13, 5, 5, 5, 13, 7, 7, 7, 15,
};

static uint8_t colour(int index, const uint8_t *cram, int type, bool tms) {
    if (tms) {                             // the TMS9918 modes: the console's own colours
        int c = index & 15;
        return c ? (uint8_t)c : C_BLACK;
    }
    if (type == SMSCV_GG) {                // RGB444 (xxxxBBBB GGGGRRRR) to the nearest 2-bit levels
        int lo = cram[index * 2], hi = cram[index * 2 + 1];
        int r = lo & 15, g = lo >> 4, b = hi & 15;
        r = (r * 3 + 7) / 15;
        g = (g * 3 + 7) / 15;
        b = (b * 3 + 7) / 15;
        return smscv_sms_colours[r | g << 2 | b << 4];
    }
    return smscv_sms_colours[cram[index] & 63];
}

// A colour's luma, 0-255, for a grey background
static uint8_t luma(int index, const uint8_t *cram, int type, bool tms) {
    static const uint8_t TMS[16] = {0, 0, 134, 170, 101, 135, 120, 185, 135, 161, 186, 204, 119, 134, 204, 255};
    if (tms) return TMS[index & 15];
    if (type == SMSCV_GG) {
        int lo = cram[index * 2], hi = cram[index * 2 + 1];
        return (uint8_t)(((lo & 15) * 77 + (lo >> 4) * 150 + (hi & 15) * 29) * 255 / (256 * 15));
    }
    int c = cram[index] & 63;
    return (uint8_t)(((c & 3) * 77 + (c >> 2 & 3) * 150 + (c >> 4) * 29) * 255 / (256 * 3));
}

// The grey background's tables from the lumas. A colour's brightness, 0-4,
// is its luma's place among the different lumas of the background's 16
// colours (the first half of the palette; the TMS9918's own in its modes):
// the darkest is black, the brightest all grey, the others spread between,
// so a dark game's background is as clear as a bright one's. Any byte but
// a sprite's pixel (bit 6 without bit 5) is then grey where its brightness
// passes its place's threshold, 0 2 / 3 1 over each 2 x 2.
static void grey_tables(smscv_t *s) {
    static const uint8_t TH[4] = {0, 2, 3, 1};
    uint8_t d[16];
    int k = 0;
    for (int i = 0; i < 16; i++) {          // the different lumas, rising
        const uint8_t l = s->luma[i];
        int j = 0;
        while (j < k && d[j] < l) j++;
        if (j < k && d[j] == l) continue;
        memmove(d + j + 1, d + j, (size_t)(k - j));
        d[j] = l;
        k++;
    }
    for (int i = 0; i < 32; i++) {
        int at = 0;
        while (at < k - 1 && d[at] < s->luma[i]) at++;
        const int t = k > 1 ? (at * 4 + (k - 1) / 2) / (k - 1) : 0;
        for (int n = 0; n < 4; n++)
            s->grey_lut[n][i] = s->grey_lut[n][0x20 | i] = s->grey_lut[n][0x60 | i] = t > TH[n] ? C_GREY : C_BLACK;
    }
    s->grey_stale = false;
}

void smscv_init(smscv_t *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 256; i++) s->lut[i] = C_BLACK;
    memset(s->grey_lut, C_BLACK, sizeof s->grey_lut);
    for (int k = 0; k < 4; k++) s->grey_lut[k][SMSCV_GREY] = C_GREY;
    s->grey_stale = true;
    s->att[0] = s->att[1] = s->att[2] = s->att[3] = 15;
    s->tone[0] = s->tone[1] = s->tone[2] = 0;
    s->noise = 0;
    for (int k = 0; k < 3; k++) s->sent_tone[k] = -1;
    for (int k = 0; k < 4; k++) s->sent_att[k] = -1;
    s->sent_noise = -1;
}

void smscv_palette(smscv_t *s, int index, const uint8_t *cram, int type, bool tms) {
    index &= 31;
    uint8_t c = colour(index, cram, type, tms);
    // every byte SMS Plus draws with this colour: the low 5 bits are the
    // colour, bits 5 and 6 its priority and sprite marks; bit 7 is the edge
    for (int hi = 0; hi < 0x80; hi += 0x20) s->lut[hi | index] = c;
    // a grey background's: a sprite's pixel as it is; the rest when a picture next wants them
    for (int k = 0; k < 4; k++) s->grey_lut[k][0x40 | index] = c;
    s->luma[index] = luma(index, cram, type, tms);
    s->grey_stale = true;
}

void smscv_frame_start(smscv_t *s, nescv_t *enc, int type) {
    s->enc = enc;
    s->gg = type == SMSCV_GG;
    s->band_at = 0;
    s->rows = 0;
    if (enc) memset(s->band, SMSCV_EDGE, sizeof s->band);
    if (enc && s->grey && s->grey_stale) grey_tables(s);
}

// A grey background: grey_lut[] makes it as the band goes in (a sprite's
// pixel that shows, bit 6 set and bit 5 clear in SMS Plus's and tms.c's
// bytes, keeps its colour; any other is black or grey, its brightness as 0
// to 4 grey pixels of each 2 x 2). Here, in each 8-pixel row of a cell with
// a sprite in it, the rest becomes all black or all grey, whichever most of
// it would be.
static void grey_line(const smscv_t *s, uint8_t *p, int line) {
    const uint8_t *const l[2] = {s->grey_lut[(line & 1) * 2], s->grey_lut[(line & 1) * 2 + 1]};
    for (int c = 0; c < SMSCV_W / 8; c++, p += 8) {
        uint32_t w[2];
        memcpy(w, p, 8);
        if (!(((w[0] >> 6 & ~(w[0] >> 5)) | (w[1] >> 6 & ~(w[1] >> 5))) & 0x01010101u)) continue;
        int rest = 0, ink = 0;
        for (int x = 0; x < 8; x++) {
            if ((p[x] & 0xE0) == 0x40) continue;
            rest++;
            ink += l[x & 1][p[x]] == C_GREY;
        }
        if (!ink || ink == rest) continue;
        const uint8_t b = ink * 2 > rest ? SMSCV_GREY : SMSCV_EDGE;
        for (int x = 0; x < 8; x++)
            if ((p[x] & 0xE0) != 0x40) p[x] = b;
    }
}

// band r to the encoder; rows not drawn are the edge
static void band_out(smscv_t *s, int r) {
    for (int y = 0; y < 8; y++) {
        if (!(s->rows >> y & 1)) memset(s->band[y], SMSCV_EDGE, SMSCV_W);
        else if (s->grey) grey_line(s, s->band[y], r * 8 + y);
    }
    if (s->grey) {
        const uint8_t *const luts[4] = {s->grey_lut[0], s->grey_lut[1], s->grey_lut[2], s->grey_lut[3]};
        nescv_band_luts(s->enc, r, &s->band[0][0], SMSCV_W, luts);
    } else {
        nescv_band(s->enc, r, &s->band[0][0], SMSCV_W, s->lut);
    }
    s->rows = 0;
}

void smscv_line(smscv_t *s, int line, const uint8_t *buf) {
    if (!s->enc || line < 0 || line >= SMSCV_H) return;
    int r = line >> 3, y = line & 7;
    while (s->band_at < r) band_out(s, s->band_at++);   // (a line skipped: never, as SMS Plus draws them)
    if (r != s->band_at) return;
    s->lines++;
    if (!buf) memset(s->band[y], SMSCV_EDGE, SMSCV_W);
    else if (s->gg) memcpy(&s->band[y][48], buf + 48, 160);   // (the edge stays round it)
    else memcpy(s->band[y], buf, SMSCV_W);
    s->rows |= (uint8_t)(1u << y);
    if (y == 7) band_out(s, s->band_at++);
}

void smscv_frame_end(smscv_t *s, uint64_t index, int reg7) {
    if (!s->enc) return;
    while (s->band_at < NESCV_ROWS) band_out(s, s->band_at++);
    // the border: the plugin's signal, else the backdrop's colour (black round a Game Gear's or a grey picture)
    nescv_band_done(s->enc, index, s->border ? s->border : s->gg || s->grey ? C_BLACK : s->lut[0x10 | (reg7 & 15)]);
    s->pictures++;
    s->enc = NULL;
}

// ---- the sound ------------------------------------------------------------------------

static void psg_write(smscv_t *s, uint8_t b) {
    s->psg_writes++;
    if (b & 0x80) s->latch = (b >> 4) & 7;
    int ch = s->latch >> 1;
    if (s->latch & 1) {                    // attenuation: the low 4 bits, latched or not
        s->att[ch] = b & 15;
    } else if (ch < 3) {                   // a tone: latched, its low 4 bits; data, its high 6
        if (b & 0x80) s->tone[ch] = (uint16_t)((s->tone[ch] & 0x3F0) | (b & 15));
        else s->tone[ch] = (uint16_t)((s->tone[ch] & 15) | (b & 0x3F) << 4);
    } else {                               // the noise: its 3 bits, latched or not
        s->noise = b & 7;
        s->noise_written = true;
    }
}

void smscv_log(smscv_t *s, int byte) {
    if (s->log_kind) {                     // the data byte of a write
        if (s->log_kind == 0x03) psg_write(s, (uint8_t)byte);
        s->log_kind = 0;
        return;
    }
    if (byte >= 0x03 && byte <= 0x06) s->log_kind = (uint8_t)byte;   // 03 PSG, 04 GG stereo, 05 / 06 FM
}

int smscv_psg(smscv_t *s, uint8_t *out) {
    int n = 0;
    for (int ch = 0; ch < 3; ch++) {
        int32_t t = s->tone[ch] ? s->tone[ch] : 1;
        // periodic noise at tone 2's rate, tone 2 silent: its period 16/15 longer
        if (ch == 2 && (s->noise & 7) == 3 && s->att[2] == 15) {
            t = (t * 16 + 7) / 15;
            if (t > 1023) t = 1023;
        }
        if (t == s->sent_tone[ch]) continue;
        out[n++] = (uint8_t)(0x80 | ch << 5 | (t & 15));
        out[n++] = (uint8_t)(t >> 4 & 0x3F);
        s->sent_tone[ch] = t;
    }
    if (s->noise_written || s->noise != s->sent_noise) {
        out[n++] = (uint8_t)(0xE0 | s->noise);
        s->sent_noise = s->noise;
        s->noise_written = false;
    }
    for (int ch = 0; ch < 4; ch++) {
        if (s->att[ch] == s->sent_att[ch]) continue;
        out[n++] = (uint8_t)(0x90 | ch << 5 | s->att[ch]);
        s->sent_att[ch] = s->att[ch];
    }
    return n;
}
