# Vectrex (VECTREX.CVP): the GCE Vectrex on the ColecoVision. The cart runs
# the machine (src/vectrex/vx.c: the 6809, the VIA, the integrators, the AY)
# from the user's own system ROM on the card (/VECTREX) and the cartridge
# the menu opened, and vxcv sends the picture and the sound to the console
# as the Game Boy kernel's lists. RESET goes back to the menu.
#
#   cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
#   ninja -C build                      -> build/VECTREX.CVP
#
# Copy VECTREX.CVP into the card's /PLUGINS folder.
cmake_minimum_required(VERSION 3.13)

# the cart's SDK: the copy in sdk/ (sdk/README.md)
set(PARROTCART_SDK ${CMAKE_CURRENT_LIST_DIR}/sdk/firmware/revc)
set(PICO_BOARD_HEADER_DIRS ${PARROTCART_SDK}/cvcart_revc/boards)
set(PICO_BOARD cvcart_revc)
set(PICO_PLATFORM rp2350-arm-s)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# the USB host: sdk/tinyusb (TinyUSB 0.21 and its fixes since), not the pico-sdk's 0.18
include(${PARROTCART_SDK}/sdk/cvpad_tinyusb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(vectrex C CXX ASM)
pico_sdk_init()
include(${PARROTCART_SDK}/sdk/cvplugin.cmake)

# The same source makes the same file, whenever it's built: the compiler's
# __DATE__ and __TIME__ (the build date in the binary info) come from
# SOURCE_DATE_EPOCH, fixed here at the build's day, 2026-11-23.
set(BUILD_EPOCH 1795435200)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})

set(S ${CMAKE_CURRENT_LIST_DIR}/src/vectrex)
set(OURS ${S}/vectrex.c ${S}/vxcard.c ${S}/vx.c ${S}/vxcv.c)
set_source_files_properties(${OURS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

add_executable(vectrex ${OURS} ${PARROTCART_SDK}/midihello/vdp.c)
target_include_directories(vectrex PRIVATE ${S} ${PARROTCART_SDK}/midihello)
target_compile_definitions(vectrex PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0 PICO_PRINTF_SUPPORT_EXPONENTIAL=0)
# -DVX_START_TEST=600: a test build that starts as 0.1.0 did, to see its
# half picture again: a name table the console lost stays lost for that
# many TV frames before it is written again, the first list is 0.1.0's
# byte for byte, and lists go out as soon as they can
if(VX_START_TEST)
  target_compile_definitions(vectrex PRIVATE VX_NAMES_HOLD=${VX_START_TEST} VX_PUBLISH_NOW=1)
endif()
cvframes_use(vectrex)
cvframes_serve2(vectrex)   # core 1's bus loop acts on two samples of the pins that agree (sdk/cvfserve.S)
cvsd_use(vectrex)
cvpad_use(vectrex)                          # USB game pads: the port is a host
pico_enable_stdio_usb(vectrex 0)
pico_enable_stdio_uart(vectrex 0)
cvcart_plugin(vectrex ID parrotcart.vectrex NAME "Vectrex" VERSION 0.1.7 BY "Parrot Cart"
              HOME /VECTREX OPENS .vec .gam USES usb-host standalone ICON src/vectrex/vectrex_icon.txt
              FILE_ICON src/vectrex/vectrex_file_icon.txt README CART-README.md FILE VECTREX.CVP)
