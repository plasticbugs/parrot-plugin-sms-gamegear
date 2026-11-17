// smscore: SMS Plus (vendor/smsplus) behind a few calls, with smscv taking
// its lines and its PSG writes. Portable C: the SMS plugin and the Mac test
// (test/sms_host.c) both run the core through it, and neither needs SMS
// Plus's own headers (whose macros don't mix with the pico-sdk's).
//
//   smscore_start(rom, size, type)    once (the core's memory comes from
//                                     frens_f_malloc, which the caller defines)
//   each TV frame:  smscore_input(...), smscore_frame(enc or NULL, index),
//                   n = smscore_psg(out)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ff.h"
#include "smscv.h"

// the controller (smscore_input): the d-pad and the two buttons
#define SMSCORE_UP    0x01
#define SMSCORE_DOWN  0x02
#define SMSCORE_LEFT  0x04
#define SMSCORE_RIGHT 0x08
#define SMSCORE_B1    0x10
#define SMSCORE_B2    0x20

extern smscv_t smscore_cv;

// the cart's type from the file's name: SMSCV_GG for .gg, SMSCV_SG for .sg
// and .sc, else SMSCV_SMS
int smscore_type_of(const char *name);

// rom: the file as it is (a copier's 512-byte header is skipped); up to 16K
// past its end may be read (a partial last bank is mapped whole)
void smscore_start(const uint8_t *rom, uint32_t size, int type);
int smscore_type(void);

// pause: the Master System's Pause button, the Game Gear's Start (held: the
// game sees one press)
void smscore_input(uint8_t pad0, uint8_t pad1, bool pause);
// a frame; its picture to enc unless NULL
void smscore_frame(nescv_t *enc, uint64_t index);
// the PSG bytes that bring the console's PSG to the game's (<= SMSCV_PSG_MAX)
int smscore_psg(uint8_t *out);

// the background on its tile grid (it scrolls 8 pixels at a time; more
// pictures a second reach the console), or as it is
void smscore_snap(bool on);

// the background (smscv.h): as the game's, grey (black and grey behind
// the sprites, which keep their colours), or off (the sprites over the
// backdrop's colour). The game runs the same in all three.
enum { SMSCORE_BG_ALL, SMSCORE_BG_GREY, SMSCORE_BG_OFF, SMSCORE_BG_N };
void smscore_bg(int mode);

// the cart's battery RAM (32K; NULL for an SG-1000 game, whose RAM isn't
// kept), and whether the game has switched it on since the start
uint8_t *smscore_sram(void);
bool smscore_sram_on(void);
#define SMSCORE_SRAM 0x8000

// a state: the core's, then the PSG as smscv has it. layout: the sizes of
// what goes in, folded (a state from another build isn't loaded)
bool smscore_save_state(FIL *f);
bool smscore_load_state(FIL *f);
uint32_t smscore_layout(void);

// where the Z80 is, for a note should the game hang: its PC and the banks
uint16_t smscore_pc(void);
uint32_t smscore_banks(void);       // the mapper's four registers, $FFFC first
uint32_t smscore_tiles_decoded(void);

// tests: each line as SMS Plus draws it, before smscv takes it
extern void (*smscore_line_hook)(int line, const uint8_t *buf);
