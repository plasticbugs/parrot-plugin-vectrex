// TinyUSB configuration for cvpad.c (a plugin's USB game pads): host only,
// on the RP2350's own USB port, through hubs (a "hub" box often holds two
// hub chips), with the HID class driver. cvpad_use() puts this directory
// first on the include path; cvpad_use(target OWN_TUSB_CONFIG) leaves it off.
#pragma once

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_PICO
#endif

#define CFG_TUH_ENABLED             1
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE 1024    // configuration and HID report descriptors
// A device that can't get an address (a hub beyond CFG_TUH_HUB) stops all
// enumeration for good in this TinyUSB (see midihello), so room to spare.
#define CFG_TUH_HUB                 3
#define CFG_TUH_DEVICE_MAX          8
#define CFG_TUH_ENDPOINT_MAX        8
#define CFG_TUH_INTERFACE_MAX       8

#define CFG_TUH_HID                 4       // HID interfaces, all devices together
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64
