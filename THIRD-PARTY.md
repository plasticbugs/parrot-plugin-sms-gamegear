# What in this repository came from elsewhere

The plugin's own code, in `src/`, is under the GNU GPL, version 3
(`LICENSE`). The copy of the cart's SDK in `sdk/` is under the MIT license
(`sdk/LICENSE`). These parts came from elsewhere and keep their own terms.

| Where | What | License |
|---|---|---|
| `vendor/smsplus/` | SMS Plus by Charles MacDonald, in the [pico-smsplus](https://github.com/PicoPlus-devel/pico-smsplus) port by Frank Hoedemakers, with the changes listed in `vendor/smsplus/VENDORED.md` | GPL-2.0-or-later (the notice in `vendor/smsplus/system.c`), and pico-smsplus as a whole GPL-3.0 (`vendor/smsplus/LICENSE-pico-smsplus`) |
| `vendor/smsplus/tms.c`, `tms.h` | SG-1000 support, from SMS Plus GX | GPL-2.0 |
| `vendor/smsplus/z80.c`, `z80.h`, `z80daa.h` | Juergen Buchmueller's portable Z80 (V2.7), from MAME, unchanged | his notice at the top of `z80.c`: "released as freeware for non-commercial purposes" |
| `sdk/firmware/revc/sdk/tinyusb/` | [TinyUSB](https://github.com/hathach/tinyusb) (Ha Thach and contributors), with the cart's fixes, listed in its `VENDORED.md` | MIT (`sdk/firmware/revc/sdk/tinyusb/LICENSE`) |
| `cmake/pico_sdk_import.cmake` | from the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | BSD 3-Clause |

The Z80 core's notice allows free redistribution, changed or not, as long
as Juergen Buchmueller is in the credits. Commercial use needs his
permission. The plugin is free, and it is never sold or bundled with the
cart.

The built plugin also contains the Raspberry Pi Pico SDK's runtime (BSD
3-Clause) and TinyUSB (MIT).

The 8x8 screen font's letter shapes come from the 1982 arcade game Mr. Do!
