/* Changed for MovieCart's ColecoVision cart, 2026-11-13: SMS_FM in place of PICO_RP2350, CACHEDTILES settable, cache_reset() in place of cacheStoreUsed. Each change is marked (cvcart); see VENDORED.md. */
#ifndef _SHARED_H_
#define _SHARED_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "loadrom.h"

#define VERSION     "0.9.4a"




#ifdef _MSC_VER
#include <stdio.h>
#include <string.h>
#ifndef __inline__
#define __inline__ __inline
#endif
#ifndef strcasecmp
#define strcasecmp stricmp
#endif
#endif
#if PICO_RP2350
#define in_ram
#endif
#ifndef in_ram
#ifdef PICO_BOARD
#include <pico.h>
#define in_ram __not_in_flash_func
#else
#define in_ram
#endif
#endif

#include "types.h"
#include "z80.h"
#include "sms.h"
#include "vdp.h"
#include "render.h"
#include "tms.h"
#include "sn76496.h"
#include "system.h"
#if SMS_FM                          /* (cvcart: was PICO_RP2350; the YM2413 is left out) */
#include "emu2413/emu2413.h"
#endif

char unalChar(const char *adr);

// Each tile takes up 8*8=64 bytes. We have 512 tiles * 4 attribs, so 2K tiles max.
#ifndef CACHEDTILES                 /* (cvcart: may be set; up to 2048) */
#define CACHEDTILES 512
#endif

extern int16 *cachePtr;                  /// [512 * 4];            // 512 * 4 *2    (tile+attr<<9) -> cache tile store index (i<<6); -1 if not cached
extern uint8 *cacheStore;                // [CACHEDTILES * 64]; // Tile store
void cache_reset(void);                  // (cvcart: the slots are render.c's own)
uint8_t *getcachestorefromemulator(size_t *size);
#ifdef __cplusplus
}
#endif

#endif /* _SHARED_H_ */
