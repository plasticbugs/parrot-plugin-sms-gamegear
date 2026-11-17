/* Changed for MovieCart's ColecoVision cart, 2026-11-13: the scroll snap (render_snap), the background left out (render_bg_off), the tile cache's slots (a free stack and a clock hand, cache_reset), sprite_over() in place of lut.h, SMS_224_SHIFT settable. Each change is marked (cvcart); see VENDORED.md. */

#include "shared.h"

/* Background drawing function */
void (*render_bg)(int line);

/* Pointer to output buffer */
uint8 *linebuf;

/* Precalculated pixel table */
//uint16 pixel[PALETTE_SIZE];


#define ALIGN_DWORD 1 // esp doesn't support unaligned word writes

// int16 cachePtr[512 * 4];            //(tile+attr<<9) -> cache tile store index (i<<6); -1 if not cached
// uint8 cacheStore[CACHEDTILES * 64]; // Tile store
// uint8 cacheStoreUsed[CACHEDTILES];  // Marks if a tile is used
int16 *cachePtr;                        //(tile+attr<<9) -> cache tile store index (i<<6); -1 if not cached
uint8 *cacheStore;                      // Tile store    

// Share memory for using in main.cpp and menu.cpp when emulator is not running
// uint8_t *getcachestorefromemulator(size_t *size) {
//     *size = CACHEDTILES * 64;
//     printf("Acquired cacheStore from emulator: %d bytes\n", *size);
//     return cacheStore;
// }
uint8 is_vram_dirty;

/* (cvcart) the scroll registers are masked with this: ~7 draws the
   background on its tile grid (it moves 8 pixels at a time, the sprites as
   they are), so the ColecoVision's encoder finds more of its cells again */
int render_snap = ~0;

/* (cvcart) nonzero: the background isn't drawn. Each line is the backdrop's
   colour (R7) with the sprites over it, all of them: no tile is in front.
   What the game reads is the same either way: the collision flag and the
   sprite overflow come from the sprites alone. */
int render_bg_off = 0;

int cacheKillPtr = 0;                   /* (cvcart) the clock hand: the slot taken next when none is free */

/* (cvcart) A sprite pixel sx over the line buffer's byte bx: what the
   64K table render_init() would make (lut.h, which pico-smsplus keeps in
   flash) worked out instead, as this plugin runs from SRAM and has no 64K to
   spare. The same for all 65536 pairs. */
static inline uint8 sprite_over(int bx, int sx)
{
    int b = bx & 0x0F, bf = bx & 0x7F, s = sx & 0x0F;
    if (bx & 0x40)                  /* over a sprite pixel: as it is */
        return bf;
    if ((bx & 0x20) && b && s)      /* hidden by a high priority tile: marked for the collision flag */
        return bf | 0x40;
    if ((bx & 0x20) && b)
        return bf;
    return s ? (s | 0x10 | 0x40) : bf;
}

/* Attribute expansion table */
uint32 atex[4] =
    {
        0x00000000,
        0x10101010,
        0x20202020,
        0x30303030,
};

/* Display sizes */
int vp_vstart;
int vp_vend;
int vp_hstart;
int vp_hend;

void render_bg_sms(int line);

void render_bg_gg(int line);

void render_obj(int line);

void palette_sync(int index);

void render_reset(void);

void render_init(void);

/* (cvcart) The tile cache's slots: a stack of the free ones, and the tile
   each one holds. It was a used flag a slot, searched from the last slot
   freed, and when every slot was taken each new tile scanned all of them
   and then dropped tiles in turn until one had been cached; and
   render_reset() cleared the tiles' entries but not the flags, so after a
   reset or a state load the cache looked full for good, and a busy game
   (Fantasy Zone) spent most of each frame scanning. Now a tile takes a free
   slot or, with none, the slot of a clock hand, at once, and cachePtr holds
   the slot's number (not its offset), so there can be more than 512. */
static int16 cacheOwner[CACHEDTILES];   /* the tile + attr << 9 in each slot; -1 free */
static int16 cacheFree[CACHEDTILES];
static int cacheFreeN;
uint32 cacheMisses;                     /* tiles decoded (for tests) */

void cache_reset(void)
{
    int i;
    for (i = 0; i < 512 * 4; i++)
        cachePtr[i] = -1;
    for (i = 0; i < CACHEDTILES; i++)
    {
        cacheOwner[i] = -1;
        cacheFree[i] = (int16)(CACHEDTILES - 1 - i);
    }
    cacheFreeN = CACHEDTILES;
    cacheKillPtr = 0;
}

void in_ram(vramMarkTileDirty)(int index)
{
    int i = index;
    while (i < 0x800)
    {
        if (cachePtr[i] != -1)
        {
            int slot = cachePtr[i];
            cacheOwner[slot] = -1;
            cacheFree[cacheFreeN++] = (int16)slot;
            cachePtr[i] = -1;
        }
        i += 0x200;
    }
}


uint8 *in_ram(getCache)(int tile, int attr)
{
    int i, x, y, c;
    int b0, b1, b2, b3;
    int i0, i1, i2, i3;
    int key = tile + (attr << 9);
    // See if we have this in cache.
    if (cachePtr[key] != -1)
        return &cacheStore[cachePtr[key] << 6];

    // Nope! Generate cache tile, in a free slot or the clock hand's.
    if (cacheFreeN)
        i = cacheFree[--cacheFreeN];
    else
    {
        i = cacheKillPtr;
        if (++cacheKillPtr >= CACHEDTILES)
            cacheKillPtr = 0;
        cachePtr[cacheOwner[i]] = -1;
    }
    cacheOwner[i] = (int16)key;
    cachePtr[key] = (int16)i;
    cacheMisses++;

    //	printf("Generating cache loc %d for tile %d attr %d\n", i, tile, attr);
    // Calculate tile
    for (y = 0; y < 8; y += 1)
    {
        b0 = vdp.vram[(tile << 5) | (y << 2) | (0)];
        b1 = vdp.vram[(tile << 5) | (y << 2) | (1)];
        b2 = vdp.vram[(tile << 5) | (y << 2) | (2)];
        b3 = vdp.vram[(tile << 5) | (y << 2) | (3)];
        for (x = 0; x < 8; x += 1)
        {
            i0 = (b0 >> (x ^ 7)) & 1;
            i1 = (b1 >> (x ^ 7)) & 1;
            i2 = (b2 >> (x ^ 7)) & 1;
            i3 = (b3 >> (x ^ 7)) & 1;

            c = (i3 << 3 | i2 << 2 | i1 << 1 | i0);
            if (attr == 0)
                cacheStore[(i << 6) | (y << 3) | (x)] = c;
            if (attr == 1)
                cacheStore[(i << 6) | (y << 3) | (x ^ 7)] = c;
            if (attr == 2)
                cacheStore[(i << 6) | ((y ^ 7) << 3) | (x)] = c;
            if (attr == 3)
                cacheStore[(i << 6) | ((y ^ 7) << 3) | (x ^ 7)] = c;
        }
    }
    return &cacheStore[i << 6];
}

/* Macros to access memory 32-bits at a time (from MAME's drawgfx.c) */

#ifdef ALIGN_DWORD

static __inline__ uint32 in_ram(read_dword)(void *address)
{
    if ((uint32)address & 3)
    {
#ifdef LSB_FIRST /* little endian version */
        return (*((uint8 *)address) +
                (*((uint8 *)address + 1) << 8) +
                (*((uint8 *)address + 2) << 16) +
                (*((uint8 *)address + 3) << 24));
#else /* big endian version */
        return (*((uint8 *)address + 3) +
                (*((uint8 *)address + 2) << 8) +
                (*((uint8 *)address + 1) << 16) +
                (*((uint8 *)address) << 24));
#endif
    }
    else
        return *(uint32 *)address;
}

static __inline__ void in_ram(write_dword)(void *address, uint32 data)
{
    if ((uint32)address & 3)
    {
#ifdef LSB_FIRST
        *((uint8 *)address) = data;
        *((uint8 *)address + 1) = (data >> 8);
        *((uint8 *)address + 2) = (data >> 16);
        *((uint8 *)address + 3) = (data >> 24);
#else
        *((uint8 *)address + 3) = data;
        *((uint8 *)address + 2) = (data >> 8);
        *((uint8 *)address + 1) = (data >> 16);
        *((uint8 *)address) = (data >> 24);
#endif
        return;
    }
    else
        *(uint32 *)address = data;
}

#else
#define read_dword(address) *(uint32 *)address
#define write_dword(address, data) *(uint32 *)address = data
#endif

/****************************************************************************/

/* Initialize the rendering data */
void render_init(void)
{
#if 0
    int bx, sx, b, s, bp, bf, sf, c;

    /* Generate 64k of data for the look up table */
    for(bx = 0; bx < 0x100; bx += 1)
    {
        for(sx = 0; sx < 0x100; sx += 1)
        {
            /* Background pixel */
            b  = (bx & 0x0F);

            /* Background priority */
            bp = (bx & 0x20) ? 1 : 0;

            /* Full background pixel + priority + sprite marker */
            bf = (bx & 0x7F);

            /* Sprite pixel */
            s  = (sx & 0x0F);

            /* Full sprite pixel, w/ palette and marker bits added */
            sf = (sx & 0x0F) | 0x10 | 0x40;

            /* Overwriting a sprite pixel ? */
            if(bx & 0x40)
            {
                /* Return the input */
                c = bf;
            }
            else if(bp && b && s)
            {
                /* A sprite pixel hidden by a high priority tile still counts
                   for the collision flag, so mark it: Fantastic Dizzy times its
                   mid-screen palette change on two sprites hidden this way. */
                c = bf | 0x40;
            }
            else
            {
                /* Work out priority and transparency for both pixels */
                c = bp ? b ? bf : s ? sf : bf : s ? sf : bf;
            }

            /* Store result (cvcart: sprite_over() works it out instead) */
            (void)c;
        }
    }

#endif
    render_reset();
}

/* Reset the rendering data */
void in_ram(render_reset)(void)
{
    int i;

    /* Clear display bitmap */
    // memset(bitmap.data, 0, bitmap.pitch * bitmap.height);
    __builtin_memset(bitmap.data, 0, bitmap.pitch);

    /* Clear palette */
    for (i = 0; i < PALETTE_SIZE; i += 1)
    {
        palette_sync(i);
    }

    /* Invalidate pattern cache (not allocated for SG-1000) */
    if (!IS_SG)
        cache_reset();

    /* Set up viewport size */
    if (IS_GG)
    {
        vp_vstart = 24;
        vp_vend = 168;
        vp_hstart = 6;
        vp_hend = 26;
    }
    else
    {
        vp_vstart = 0;
        vp_vend = 192;
        vp_hstart = 0;
        vp_hend = 32;
    }

    /* Pick render routine */
    render_bg = IS_GG ? render_bg_gg : render_bg_sms;
}

extern void sms_render_line(int line, const uint8_t *buffer);

/* (cvcart) the lines a 224-line Master System picture loses at the top: 6
   for main.cpp's 212, 16 for the ColecoVision's 192 */
#ifndef SMS_224_SHIFT
#define SMS_224_SHIFT 6
#endif

/* The viewport and output line offset of the current frame. They are fixed at
   its first line, so that every frame hands sms_render_line() the same run of
   line numbers even when a game changes mode halfway down: in line-buffer mode
   the DVI driver takes one line per call. */
static int frame_vstart, frame_vend, frame_shift;

/* Draw a line of the display */
void in_ram(render_line)(int line)
{
    if (line == 0)
    {
        if (IS_224_MODE)
        {
            /* main.cpp shows 212 lines, so a 224-line Master System picture
               loses 6 at the top and 6 at the bottom. The Game Gear screen shows
               the same window of the TV picture as in the 192-line mode, which
               starts at active line 40 instead of 24. */
            frame_shift = IS_GG ? 16 : SMS_224_SHIFT;
            frame_vstart = IS_GG ? vp_vstart + 16 : 0;
            frame_vend = IS_GG ? vp_vend + 16 : 224;
        }
        else
        {
            frame_shift = 0;
            frame_vstart = vp_vstart;
            frame_vend = vp_vend;
        }
    }

    /* Ensure we're within the viewport range */
    if ((line < frame_vstart) || (line >= frame_vend)) {
        // Hack to render emulator display center correctly vertically by drawing black lines on top and bottom.
        sms_render_line(line - frame_shift, 0);
        return;
    }

    /* Point to current line in output buffer */
    // linebuf = &bitmap.data[(line * bitmap.pitch)];
    linebuf = &bitmap.data[0];

    /* The TMS9918A modes: every SG-1000 line, and a Master System or Game Gear
       line drawn with R0 bit 2 clear - the video chip starts in these modes and
       the conversions of MSX games stay in them. The whole 256-byte row is
       written; the Game Gear blit only shows its 160-pixel window of it. */
    if (IS_TMS_MODE)
    {
        /* TMS9918A: R1 bit 6 enables the display */
        if (vdp.reg[1] & 0x40)
        {
            if (render_bg_off)          /* (cvcart) */
                __builtin_memset(linebuf, BACKDROP_COLOR, SMS_WIDTH);
            else
                tms_render_bg(line);
            tms_render_obj(line);
        }
        else
        {
            __builtin_memset(linebuf, BACKDROP_COLOR, SMS_WIDTH);
        }
        sms_render_line(line - frame_shift, linebuf);
        return;
    }

    /* Blank line. R2 bit 0 clear blanks a Master System line only in the
       192-line mode: the 224-line mode takes its name table from R2 bits 2-3,
       and Micro Machines leaves bit 0 clear. */
    if ((!(vdp.reg[1] & 0x40)) || (((vdp.reg[2] & 1) == 0) && (IS_SMS) && !IS_224_MODE))
    {
        __builtin_memset(linebuf + (vp_hstart << 3), BACKDROP_COLOR, BMP_WIDTH);
    }
    else
    {
        /* Draw background */
        if (render_bg_off)              /* (cvcart) */
            __builtin_memset(linebuf + (vp_hstart << 3), BACKDROP_COLOR, BMP_WIDTH);
        else
            render_bg(line);

        /* Draw sprites */
        render_obj(line);

        /* Blank leftmost column of display */
        if (vdp.reg[0] & 0x20)
        {
            __builtin_memset(linebuf, BACKDROP_COLOR, 8);
        }
    }

    sms_render_line(line - frame_shift, linebuf);
    // if ( line == 191) {
    //     __builtin_memset(linebuf, 0, SMS_WIDTH);
    //     for (int i = line ; i <= 192 ; i++) {
    //         sms_render_line(i, linebuf);
    //     }
    // }
}

/* Draw the Master System background */
void in_ram(render_bg_sms)(int line)
{
    int locked = 0;
    /* The name table is 28 rows tall, or 32 in the 224-line mode */
    int v_line = line + (vdp.reg[9] & render_snap);
    if (IS_224_MODE)
        v_line &= 0xFF;
    else if (v_line >= 224)
        v_line -= 224;
    int v_row = (v_line & 7) << 3;
    int hscroll = ((vdp.reg[0] & 0x40) && (line < 0x10)) ? 0 : ((0x100 - vdp.reg[8]) & render_snap);
    int column = vp_hstart;
    uint16 attr;
    uint16 *nt = (uint16 *)&vdp.vram[vdp.ntab + ((v_line >> 3) << 6)];
    int nt_scroll = (hscroll >> 3);
    int shift = (hscroll & 7);
    uint32 atex_mask;
    uint32 *cache_ptr;
    uint32 *linebuf_ptr = (uint32 *)&linebuf[0 - shift];
    uint8 *ctp;

    /* Draw first column (clipped) */
    if (shift)
    {
        int x, c, a;

        attr = nt[(column + nt_scroll) & 0x1F];

#ifndef LSB_FIRST
        attr = (((attr & 0xFF) << 8) | ((attr & 0xFF00) >> 8));
#endif
        a = (attr >> 7) & 0x30;

        for (x = shift; x < 8; x += 1)
        {
            ctp = getCache((attr & 0x1ff), (attr >> 9) & 3);
            c = ctp[(v_row) | (x)];
            linebuf[(0 - shift) + (x)] = ((c) | (a));
        }

        column += 1;
    }

    /* Draw a line of the background */
    for (; column < vp_hend; column += 1)
    {
        /* Stop vertical scrolling for leftmost eight columns */
        if ((vdp.reg[0] & 0x80) && (!locked) && (column >= 24))
        {
            locked = 1;
            v_row = (line & 7) << 3;
            nt = (uint16 *)&vdp.vram[((vdp.reg[2] << 10) & 0x3800) + ((line >> 3) << 6)];
        }

        /* Get name table attribute word */
        attr = nt[(column + nt_scroll) & 0x1F];

#ifndef LSB_FIRST
        attr = (((attr & 0xFF) << 8) | ((attr & 0xFF00) >> 8));
#endif
        /* Expand priority and palette bits */
        atex_mask = atex[(attr >> 11) & 3];

        /* Point to a line of pattern data in cache */
        ctp = getCache((attr & 0x1ff), (attr >> 9) & 3);
        cache_ptr = (uint32 *)&ctp[(v_row)];

        /* Copy the left half, adding the attribute bits in */
        write_dword(&linebuf_ptr[(column << 1)], read_dword(&cache_ptr[0]) | (atex_mask));

        /* Copy the right half, adding the attribute bits in */
        write_dword(&linebuf_ptr[(column << 1) | (1)], read_dword(&cache_ptr[1]) | (atex_mask));
    }

    /* Draw last column (clipped) */
    if (shift)
    {
        int x, c, a;

        char *p = &linebuf[(0 - shift) + (column << 3)];

        attr = nt[(column + nt_scroll) & 0x1F];

#ifndef LSB_FIRST
        attr = (((attr & 0xFF) << 8) | ((attr & 0xFF00) >> 8));
#endif
        a = (attr >> 7) & 0x30;

        for (x = 0; x < shift; x += 1)
        {
            ctp = getCache((attr & 0x1ff), (attr >> 9) & 3);
            c = ctp[(v_row) | (x)];
            p[x] = ((c) | (a));
        }
    }
}

/* Draw the Game Gear background */
void render_bg_gg(int line)
{
    /* The name table is 28 rows tall, or 32 in the 224-line mode */
    int v_line = line + (vdp.reg[9] & render_snap);
    if (IS_224_MODE)
        v_line &= 0xFF;
    else if (v_line >= 224)
        v_line -= 224;
    int v_row = (v_line & 7) << 3;
    int hscroll = ((0x100 - vdp.reg[8]) & render_snap);
    int column;
    uint16 attr;
    uint16 *nt = (uint16 *)&vdp.vram[vdp.ntab + ((v_line >> 3) << 6)];
    int nt_scroll = (hscroll >> 3);
    uint32 atex_mask;
    uint32 *cache_ptr;
    uint32 *linebuf_ptr = (uint32 *)&linebuf[0 - (hscroll & 7)];
    uint8_t *ctp;

    /* Draw a line of the background */
    for (column = vp_hstart; column <= vp_hend; column += 1)
    {
        /* Get name table attribute word */
        attr = nt[(column + nt_scroll) & 0x1F];

#ifndef LSB_FIRST
        attr = (((attr & 0xFF) << 8) | ((attr & 0xFF00) >> 8));
#endif
        /* Expand priority and palette bits */
        atex_mask = atex[(attr >> 11) & 3];

        /* Point to a line of pattern data in cache */
        ctp = getCache((attr & 0x1ff), (attr >> 9) & 3);
        cache_ptr = (uint32 *)&ctp[(v_row)];

        /* Copy the left half, adding the attribute bits in */
        write_dword(&linebuf_ptr[(column << 1)], read_dword(&cache_ptr[0]) | (atex_mask));

        /* Copy the right half, adding the attribute bits in */
        write_dword(&linebuf_ptr[(column << 1) | (1)], read_dword(&cache_ptr[1]) | (atex_mask));
    }
}

/* Draw sprites */
void in_ram(render_obj)(int line)
{
    int i;
    uint8_t *ctp;

    /* Sprite count for current line (8 max.) */
    int count = 0;

    /* Sprite dimensions */
    int width = 8;
    int height = (vdp.reg[1] & 0x02) ? 16 : 8;

    /* Pointer to sprite attribute table */
    uint8 *st = (uint8 *)&vdp.vram[vdp.satb];

    /* Y = 208 ends the sprite list, but not in the 224-line mode, where it is
       a line on screen */
    int end_marker = IS_224_MODE ? -1 : 208;

    /* Adjust dimensions for double size sprites */
    if (vdp.reg[1] & 0x01)
    {
        width *= 2;
        height *= 2;
    }

    /* Draw sprites in front-to-back order */
    for (i = 0; i < 64; i += 1)
    {
        /* Sprite Y position */
        int yp = st[i];

        /* End of sprite list marker? */
        if (yp == end_marker)
            return;

        /* Actual Y position is +1 */
        yp += 1;

        /* Wrap Y coordinate for sprites > 240 */
        if (yp > 240)
            yp -= 256;

        /* Check if sprite falls on current line */
        if ((line >= yp) && (line < (yp + height)))
        {
            uint8 *linebuf_ptr;

            /* Width of sprite */
            int start = 0;
            int end = width;

            /* Sprite X position */
            int xp = st[0x80 + (i << 1)];

            /* Pattern name */
            int n = st[0x81 + (i << 1)];

            /* Bump sprite count */
            count += 1;

            /* Too many sprites on this line ? */
            if ((vdp.limit) && (count == 9))
                return;

            /* X position shift */
            if (vdp.reg[0] & 0x08)
                xp -= 8;

            /* Add MSB of pattern name */
            if (vdp.reg[6] & 0x04)
                n |= 0x0100;

            /* Mask LSB for 8x16 sprites */
            if (vdp.reg[1] & 0x02)
                n &= 0x01FE;

            /* Point to offset in line buffer */
            linebuf_ptr = (uint8 *)&linebuf[xp];

            /* Clip sprites on left edge */
            if (xp < 0)
            {
                start = (0 - xp);
            }

            /* Clip sprites on right edge */
            if ((xp + width) > 256)
            {
                end = (256 - xp);
            }

            /* Draw double size sprite */
            if (vdp.reg[1] & 0x01)
            {
                int x;
                ctp = getCache((n & 0x1ff) + ((line - yp) >> 3), (n >> 9) & 3);
                uint8 *cache_ptr = (uint8 *)&ctp[(((line - yp) >> 1) << 3)];

                /* Draw sprite line */
                for (x = start; x < end; x += 1)
                {
                    /* Source pixel from cache */
                    uint8 sp = cache_ptr[(x >> 1)];

                    /* Only draw opaque sprite pixels */
                    if (sp)
                    {
                        /* Background pixel from line buffer */
                        uint8 bg = linebuf_ptr[x];

                        /* Look up result */
                        linebuf_ptr[x] = sprite_over(bg, sp);

                        /* Set sprite collision flag */
                        if (bg & 0x40)
                            vdp.status |= 0x20;
                    }
                }
            }
            else /* Regular size sprite (8x8 / 8x16) */
            {
                int x;
                ctp = getCache((n & 0x1ff) + ((line - yp) >> 3), (n >> 9) & 3);
                uint8 *cache_ptr = (uint8 *)&ctp[((line - yp) << 3) & 0x38];

                /* Draw sprite line */
                for (x = start; x < end; x += 1)
                {
                    /* Source pixel from cache */
                    uint8 sp = cache_ptr[x];

                    /* Only draw opaque sprite pixels */
                    if (sp)
                    {
                        /* Background pixel from line buffer */
                        uint8 bg = linebuf_ptr[x];

                        /* Look up result */
                        linebuf_ptr[x] = sprite_over(bg, sp);

                        /* Set sprite collision flag */
                        if (bg & 0x40)
                            vdp.status |= 0x20;
                    }
                }
            }
        }
    }
}

/* Update pattern cache with modified tiles */

extern void sms_palette_sync(int index);
extern void sms_palette_syncGG(int index);
extern void sms_palette_syncSG(int index);

/* Update a palette entry */
void in_ram(palette_sync)(int index)
{
    // FH: Changed begin
#if 0
    int r, g, b;

    if (IS_GG)
    {
        r = ((vdp.cram[(index << 1) | 0] >> 1) & 7) << 5;
        g = ((vdp.cram[(index << 1) | 0] >> 5) & 7) << 5;
        b = ((vdp.cram[(index << 1) | 1] >> 1) & 7) << 5;
    }
    else
    {
        r = ((vdp.cram[index] >> 0) & 3) << 6;
        g = ((vdp.cram[index] >> 2) & 3) << 6;
        b = ((vdp.cram[index] >> 4) & 3) << 6;
    }
   
    bitmap.pal.color[index][0] = r;
    bitmap.pal.color[index][1] = g;
    bitmap.pal.color[index][2] = b;
  
    pixel[index] = MAKE_PIXEL(r, g, b);
#endif
    // FH: Changed end

    bitmap.pal.dirty[index] = bitmap.pal.update = 1;
    if (IS_TMS_MODE)
    {
        /* Fixed TMS9918A palette. The Master System has no CRAM colours in
           these modes either; vdp_ctrl_w() syncs the whole palette again when
           a game switches between them and Mode 4. */
        sms_palette_syncSG(index);
    }
    else if (IS_GG)
    {
        sms_palette_syncGG(index);
    }
    else
    {
        sms_palette_sync(index);
    }
}
