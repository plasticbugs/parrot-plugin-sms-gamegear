# SMS & Game Gear: Sega Master System, Game Gear and SG-1000 games on the
# ColecoVision. Pick a .sms, .gg or .sg in the menu and press A: SMS Plus
# (vendor/smsplus, from pico-smsplus) runs it on the cart, and smscv and
# nescv send each picture and the sound to the console as the Game Boy
# kernel's lists. RESET goes back to the menu.
#
#   cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
#   ninja -C build                      -> build/SMS.CVP
#
# Copy SMS.CVP into the card's /PLUGINS folder.
cmake_minimum_required(VERSION 3.13)

# the cart's SDK: the copy in sdk/ (sdk/README.md)
set(PARROTCART_SDK ${CMAKE_CURRENT_LIST_DIR}/sdk/firmware/revc)
set(PICO_BOARD_HEADER_DIRS ${PARROTCART_SDK}/cvcart_revc/boards)
set(PICO_BOARD cvcart_revc)
set(PICO_PLATFORM rp2350-arm-s)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# the USB host: TinyUSB 0.21.0 (sdk/tinyusb), not the pico-sdk's 0.18, whose
# tuh_task() stops the game for half a second and more when a pad is plugged
# in and can panic. -DCVPAD_TINYUSB_SDK=ON: 0.18.
include(${PARROTCART_SDK}/sdk/cvpad_tinyusb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(sms C CXX ASM)
pico_sdk_init()
include(${PARROTCART_SDK}/sdk/cvplugin.cmake)

# The same source makes the same file, whenever it's built: the compiler's
# __DATE__ and __TIME__ (the build date in the binary info) come from
# SOURCE_DATE_EPOCH, fixed here at the build's day, 2026-11-23.
set(BUILD_EPOCH 1795435200)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})

set(S ${CMAKE_CURRENT_LIST_DIR}/src)
file(GLOB CORE ${CMAKE_CURRENT_LIST_DIR}/vendor/smsplus/*.c)
# vendored: its own warnings aren't ours to fix
set_source_files_properties(${CORE} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(src/sms.c ${S}/smscv.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
# (SMS Plus's sms.h defines statics that smscore.c doesn't use)
set_source_files_properties(${S}/smscore.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-variable")

add_executable(sms src/sms.c ${S}/smscore.c ${S}/smscv.c ${S}/nescv.c ${CORE})
target_include_directories(sms PRIVATE ${S} ${S}/port ${CMAKE_CURRENT_LIST_DIR}/vendor/smsplus ${PARROTCART_SDK}/midihello)
# LSB_FIRST: the Z80's register pairs, little-endian (as pico-smsplus has it).
# SMS_224_SHIFT: the 224-line mode loses 16 lines at the top and the bottom.
# CACHEDTILES: SMS Plus's tile cache, 64 bytes a tile.
# NESCV_LUTS: nescv_band_luts(), for the grey background's dots (smscv.h).
target_compile_definitions(sms PRIVATE LSB_FIRST=1 SMS_224_SHIFT=16 CACHEDTILES=512 NESCV_LUTS)
# RAM: the plugin runs from SRAM, and SMS Plus's memory is what's left past
# .bss. The SDK's printf without floats (nothing here prints one): 2K.
target_compile_definitions(sms PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0 PICO_PRINTF_SUPPORT_EXPONENTIAL=0)
cvframes_use(sms)
cvframes_serve2(sms)   # core 1's bus loop acts on two samples of the pins that agree (sdk/cvfserve.S)
cvsd_use(sms)
cvpsram_use(sms)
cvpad_use(sms)                              # USB game pads: the port is a host
pico_enable_stdio_usb(sms 0)
pico_enable_stdio_uart(sms 0)
cvcart_plugin(sms ID parrotcart.mastersystem NAME "SMS & Game Gear" VERSION 0.2.8 BY "Parrot Cart"
              HOME /SMS OPENS .sms .gg .sg USES usb-host psram ICON src/sms_icon.txt FILE_ICON src/sms_file_icon.txt
              README CART-README.md)
