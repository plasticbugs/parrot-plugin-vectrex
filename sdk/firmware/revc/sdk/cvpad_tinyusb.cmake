# cvpad_tinyusb.cmake -- a plugin's USB host from sdk/tinyusb (TinyUSB 0.21.0)
# instead of the pico-sdk's 0.18: include it before pico_sdk_import.cmake.
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../sdk/cvpad_tinyusb.cmake)
#   include(pico_sdk_import.cmake)
#
# 0.18's host blocks tuh_task() for half a second at a time while a device
# enumerates, and panics on a late buffer (sdk/tinyusb/VENDORED.md).
# -DCVPAD_TINYUSB_SDK=ON builds with the pico-sdk's again, to compare.
# -DPICO_TINYUSB_PATH=dir builds with another checkout of TinyUSB (0.21 or
# later; its hub.c is then its own, without the VL817 fix).
option(CVPAD_TINYUSB_SDK "USB host from the pico-sdk's TinyUSB 0.18" OFF)
if (NOT CVPAD_TINYUSB_SDK)
    if (NOT PICO_TINYUSB_PATH)
        set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/tinyusb)
    endif()
    set(CVPAD_TINYUSB_VENDORED TRUE)
endif()
