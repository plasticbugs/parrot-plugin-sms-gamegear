/* Changed for MovieCart's ColecoVision cart, 2026-11-13: render_snap, render_bg_off and cacheMisses exported. Each change is marked (cvcart); see VENDORED.md. */

#ifndef _RENDER_H_
#define _RENDER_H_

/* Pack RGB data into a 16-bit RGB 5:6:5 format */
#define MAKE_PIXEL(r, g, b)   (((r << 8) & 0xF800) | ((g << 3) & 0x07E0) | ((b >> 3) & 0x001F))

/* Used for blanking a line in whole or in part */
#define BACKDROP_COLOR      (0x10 | (vdp.reg[7] & 0x0F))


/* Function prototypes */
void render_init(void);

void render_reset(void);

void render_bg_gg(int line);

void render_bg_sms(int line);

void render_obj(int line);

void render_line(int line);

void update_cache(void);

void palette_sync(int index);

void remap_8_to_16(int line);

void vramMarkTileDirty(int tile);

extern int render_snap;    /* (cvcart) ~0, or ~7: the background on its tile grid */
extern int render_bg_off;  /* (cvcart) nonzero: no background, the sprites over the backdrop */
extern uint32 cacheMisses; /* (cvcart) tiles decoded */

#endif /* _RENDER_H_ */
