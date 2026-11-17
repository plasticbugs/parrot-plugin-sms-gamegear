# SMS Plus (pico-smsplus), vendored

- Upstream: https://github.com/PicoPlus-devel/pico-smsplus, `smsplus/`,
  commit `51432b2ef1a13c33ba38c5b543fbeb776bab88f6`. Copied 2026-11-12.
- Origin: SMS Plus by Charles MacDonald (1998-2007), ported to the Pico
  by Frank Hoedemakers (https://github.com/fhoedemakers/pico-smsplus; the
  credit in upstream's README). pico-smsplus adds the SG-1000 (`tms.c`,
  from SMS Plus GX), the line-buffer renderer, the tile cache and the
  YM2413 (FM) on the RP2350.
- Licences:
  - pico-smsplus as a whole: GPL-3 (`LICENSE-pico-smsplus`).
  - SMS Plus's files (`system.c` and the rest without a header of their
    own): Charles MacDonald, GPL-2-or-later (the text in `system.c`);
    `tms.c` / `tms.h`: from SMS Plus GX, GPL-2.
  - `z80.c`, `z80.h`, `z80daa.h`: Juergen Buchmueller's portable Z80 (from
    MAME, V2.7), "released as freeware for non-commercial purposes", his
    notice at the top of `z80.c`, unchanged here. It allows free
    redistribution, changed or not, so long as he is listed in the credits
    and changed files say so (this copy is unchanged); commercial use needs
    his permission.
- How the plugins go out: free, as separate
  downloads that each owner picks, never bundled with the cart. That is
  the non-commercial use the Z80's notice allows. Each release of SMS.CVP
  should carry:
  - the credits: SMS Plus by Charles MacDonald, the Pico port by Frank
    Hoedemakers (pico-smsplus), the Z80 core by Juergen Buchmueller;
  - the source: SMS.CVP, built from these with our own code (smscv,
    smscore, nescv, the SDK it links), is GPL-3 as a whole, so whoever gets
    it is owed all of that source (a link to it beside the download does).
- Should a commercial use ever come up, a Z80 under a plain licence
  (superzazu/z80, MIT) would take the MAME core's place.

## Kept

`cpuintrf.h`, `loadrom.c/.h`, `osd_cpu.h`, `render.c/.h`, `shared.h`,
`sms.c/.h`, `sn76496.c/.h`, `system.c/.h`, `tms.c/.h`, `types.h`,
`vdp.c/.h`, `z80.c/.h`, `z80daa.h`.

## Left out

- `lut.h`: the 64K sprite-over-background table, kept in flash upstream;
  this plugin runs from SRAM (see `sprite_over()` below).
- `emu2413/`, `vrc7tone.h`: the YM2413 (the Japanese Master System's FM
  sound). The ColecoVision has no FM; the games' PSG music plays instead
  (a game asks for FM only when it finds the chip, and SMS Plus says it
  isn't there).
- `CMakeLists.txt`, and all of pico-smsplus outside `smsplus/`: `main.cpp`
  (HDMI, I2S, the menu, FatFS, its pad drivers), `pico_shared/`, the board
  files and builds.

## Changes

Every change is marked `(cvcart)` in the source, and each changed file
says so at its top; everything else is byte-identical to upstream.

- **`render.c`**
  - `render_snap`: the background's scroll (registers 8 and 9) masked
    with it, in `render_bg_sms` and `render_bg_gg`. `~0` (the start) draws
    the game's picture as it is; `~7` puts the background on its tile grid,
    so it moves 8 pixels at a time (the sprites as they are) and the
    ColecoVision's encoder finds more of its cells already on the console.
  - The tile cache's slots: a stack of the free ones (`cacheFree`), the
    tile each holds (`cacheOwner`), and a clock hand (`cacheKillPtr`) for a
    tile with no slot free; `cachePtr` holds the slot's number, not its
    offset, so `CACHEDTILES` can pass 512. `cache_reset()` empties it
    (`render_reset()` calls it). Upstream kept a used flag a slot that
    `render_reset()` didn't clear, so after a reset or a state load the
    cache looked full for good and each new tile scanned every slot:
    Fantasy Zone then spent most of a frame there and the watchdog
    rebooted the cart. `cacheMisses` counts the tiles decoded.
  - `sprite_over(bx, sx)`: a sprite pixel over a line-buffer byte, worked
    out in place of the `lut.h` table (checked: the same for all 65536
    pairs). `render_init()` still runs its loop and throws the result away.
  - `SMS_224_SHIFT` (6 upstream, for its 212 lines): the lines a 224-line
    picture loses at the top. The plugin sets 16, for 192.
  - `render_bg_off` (0.2.0): nonzero, `render_line()` doesn't draw the
    background (`render_bg`, or `tms_render_bg` in the TMS9918 modes): the
    line is the backdrop's colour and the sprites go over it. The sprite
    collision and overflow flags come from the sprites alone, so the game
    runs the same.
- **`render.h`**: `render_snap`, `render_bg_off` and `cacheMisses` exported.
- **`sms.c`** (0.2.0): `sg_open_bus`, 8K of `$FF` for an SG-1000 game's
  unmapped pages, was a const table ("kept in flash" upstream, but in SRAM
  here, for every game); now `frens_f_malloc`'d the first time an SG-1000
  map needs it.
- **`shared.h`**: `#if SMS_FM` in place of `#if PICO_RP2350` (the YM2413
  is left out on the RP2350 too); `CACHEDTILES` may be set from outside;
  `cacheStoreUsed` gone, `cache_reset()` declared.
- **`system.c`**: the same `SMS_FM`; `cacheStoreUsed` no longer allocated
  or freed; `system_load_state()`'s 64-byte register buffer is static
  (upstream allocated it each load and lost it when a load failed).

## Building it

- `-DLSB_FIRST=1`: the Z80's register pairs and the renderer, little-endian.
  The core tests it with `#ifdef`, so pico-smsplus's `-DLSB_FIRST=0` sets it
  too; left undefined, the big-endian code is built.
- `CACHEDTILES=512`, `SMS_224_SHIFT=16` (`plugin/CMakeLists.txt`).
- The core calls out to `sms_render_line()`, `sms_palette_sync()`,
  `sms_palette_syncGG()`, `sms_palette_syncSG()`, `system_load_sram()` and
  `frens_f_malloc()` / `frens_f_free()`; `../../smscore.c` defines the first
  five and the plugin (or `test/sms_host.c`) the allocator. Its states use
  FatFS's `FIL`, `f_read` and `f_write`: `../../port/ff.h` is a stand-in
  that calls back into the plugin.
- `-w`: its own warnings are left alone.
