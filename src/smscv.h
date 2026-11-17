// smscv: a Master System, Game Gear or SG-1000 game's picture and sound as
// the ColecoVision takes them. Portable C (no pico-sdk), used by the SMS
// plugin and by the Mac test (test/sms_host.c).
//
// The picture goes through nescv (../nes/nescv.h): the console runs
// gbkernel and nescv turns 256 x 192 pictures into its command lists. The
// Master System's picture is 256 x 192 too, so it goes across at 1:1, line
// for line: SMS Plus draws a line at a time (render.c's sms_render_line),
// and each band of 8 lines goes to the encoder as it's done, with no
// picture buffer. A Game Gear's 160 x 144 sits in the middle, where it is
// in the Master System's picture anyway, with black around it; the
// 224-line mode loses 16 lines at the top and 16 at the bottom.
//
// Colours: the Master System's 64 (2 bits each of red, green and blue) go
// to the nearest of the TMS9918's 15, the greys to black, grey and white
// only; the Game Gear's 4096 to the nearest of the 64 first. The TMS9918
// modes, which the SG-1000 games and some early Master System ones use, are
// the console's own: their colours go across as they are. Each 8-pixel row
// of a cell then holds two colours (nescv picks them).
//
// The background (the plugin's keypad 1): as the game's; grey (smscv_t's
// grey); or off (SMS Plus's render_bg_off: the sprites over the backdrop's
// colour). Grey: the sprites keep their colours and everything else is
// drawn in black and grey, a colour's brightness as 0 to 4 grey pixels of
// each 2 x 2 (the dots sit on the screen, not on the background, so a
// scrolling shade stays as it is). The brightness is the colour's place
// among the background palette's, darkest to brightest, not its luma, so
// a dark game's background shows as well as a bright one's; it follows the
// palette from the next picture on (a palette changed mid-frame greys both
// parts of the picture by the later one). A row of a cell then never has to choose
// among the background's colours, and where a sprite is in the row the rest
// of it is all black or all grey (whichever most of it was): the row's two
// colours are the sprite's first. The game runs the same in all three.
// Grey costs the cart little: the dots come from four tables in place of
// lut (nescv_band_luts, so nescv is built with NESCV_LUTS), and only a
// cell with a sprite in it is gone over.
//
// The sound: the Master System's PSG is the ColecoVision's (an SN76489 at
// the same clock), so its writes, gathered over the frame, go across as the
// registers they leave: each TV frame, the ones that changed. Two
// differences are made up for: a tone period of 0 is 1 on Sega's chip (and
// 1024 on the ColecoVision's), and periodic noise clocked by tone 2 runs 16
// steps on Sega's and 15 on the ColecoVision's, so while tone 2 is silent
// its period goes across 16/15 longer (the noise then plays at the pitch it
// should). The Game Gear's stereo and the Japanese consoles' FM are left
// out.
//
//   once:                   smscv_init(s)
//   palette changes:        smscv_palette(s, index, cram, type)
//   each frame:             smscv_frame_start(s, enc or NULL)  (NULL: no picture this frame)
//                           sms_frame(0), whose lines reach smscv_line(s, line, buf)
//                           smscv_frame_end(s, index, vdp.reg[7])
//   PSG writes (snd.callback, snd.log = 1):  smscv_log(s, byte)
//   each TV frame:          n = smscv_psg(s, out)   (<= SMSCV_PSG_MAX bytes)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "nescv.h"

#define SMSCV_W 256
#define SMSCV_H 192
#define SMSCV_EDGE 0x80          // a pixel outside the picture (SMS Plus never draws bit 7)
#define SMSCV_GREY 0x81          // a grey pixel (the edge's byte is black); grey backgrounds only
#define SMSCV_PSG_MAX 11

enum { SMSCV_SMS, SMSCV_GG, SMSCV_SG };   // as SMS Plus's TYPE_SMS, TYPE_GG, TYPE_SG

// the Master System's colours (index R | G << 2 | B << 4) as TMS9918 colours (1-15)
extern const uint8_t smscv_sms_colours[64];

typedef struct {
    // the picture
    nescv_t *enc;                // this frame's picture goes to it (NULL: none)
    uint8_t lut[256];            // a line buffer's byte as a TMS9918 colour
    uint8_t band[8][SMSCV_W];
    int band_at;                 // the band being drawn
    uint8_t rows;                // its rows drawn so far (a bit each)
    bool gg;                     // a Game Gear: its 160 x 144 only
    bool grey;                   // all but the sprites in black and grey
    uint8_t grey_lut[4][256];    // lut for that, by the pixel's place: [(y & 1) * 2 + (x & 1)]
    uint8_t luma[32];            // the palette's lumas
    bool grey_stale;             // grey_lut is behind them
    uint8_t border;              // 0, or a colour the border shows instead of the game's (the plugin's signals)
    uint32_t lines, pictures;    // stats

    // the sound: the PSG's registers, as the game left them and as sent
    uint8_t log_kind;            // the snd.log byte waiting for its data (0: none)
    uint8_t latch;               // the register the next data byte goes to (0-7)
    uint16_t tone[3];
    uint8_t att[4];
    uint8_t noise;
    bool noise_written;          // (a write restarts the noise: it goes across each time)
    int32_t sent_tone[3];        // -1: never sent
    int16_t sent_att[4];
    int16_t sent_noise;
    uint32_t psg_writes;
} smscv_t;

void smscv_init(smscv_t *s);
// colour `index` (0-31) has changed: cram is the VDP's CRAM (vdp.cram), type
// the cart's; in the TMS9918 modes (tms true) the colours are fixed
void smscv_palette(smscv_t *s, int index, const uint8_t *cram, int type, bool tms);
void smscv_frame_start(smscv_t *s, nescv_t *enc, int type);
// SMS Plus's sms_render_line(line, buf): buf NULL for a line outside the
// picture; lines outside 0-191 are let be
void smscv_line(smscv_t *s, int line, const uint8_t *buf);
// the frame's picture to the encoder (if it was taking one); reg7 the VDP's
// R7, whose low 4 bits are the border's colour (16 + n in Mode 4, n in the
// TMS9918 modes: colour 16 + n is n there)
void smscv_frame_end(smscv_t *s, uint64_t index, int reg7);

void smscv_log(smscv_t *s, int byte);
int smscv_psg(smscv_t *s, uint8_t *out);
