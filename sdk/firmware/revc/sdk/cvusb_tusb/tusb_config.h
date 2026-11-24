// TinyUSB's configuration for cvusb (cvusb.h): only the classes a plugin's
// features need, as cvcart_features() switches them on (CVUSB_PADS ...:
// compile definitions). cvcart_features() puts this directory first on the
// include path. TinyUSB is sdk/tinyusb (0.21 and its fixes).
#pragma once

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_PICO
#endif

// ---- a host: pads, keyboards, mice, MIDI devices, through hubs ---------------
#if CVUSB_PADS || CVUSB_KEYBOARD || CVUSB_MOUSE || CVUSB_MIDI_IN
#define CFG_TUH_ENABLED             1
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE 1024    // configuration and HID report descriptors
// A device that can't get an address (a hub beyond CFG_TUH_HUB) stops all
// enumeration for good in this TinyUSB (see midihello), so room to spare: a
// "hub" box often holds two hub chips.
#define CFG_TUH_HUB                 3
#define CFG_TUH_DEVICE_MAX          8
#define CFG_TUH_ENDPOINT_MAX        8
#define CFG_TUH_INTERFACE_MAX       8

// HID interfaces, all devices together: a pad has one, a keyboard often two
// (its keys, and its media keys), a mouse one or two
#if CVUSB_PADS && (CVUSB_KEYBOARD || CVUSB_MOUSE)
#define CFG_TUH_HID                 8
#elif CVUSB_KEYBOARD || CVUSB_MOUSE
#define CFG_TUH_HID                 6
#elif CVUSB_PADS
#define CFG_TUH_HID                 4
#endif
#ifdef CFG_TUH_HID
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64
#endif
// MIDI_IN is cvusb_midih.c, an application driver (PSG Synth's): TinyUSB's
// own MIDI host stays off (CFG_TUH_MIDI 0)
#endif

// ---- a device: MIDI to a computer ------------------------------------------------
#if CVUSB_MIDI_OUT
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64
#define CFG_TUD_MIDI                1
#define CFG_TUD_MIDI_RX_BUFSIZE     64
#define CFG_TUD_MIDI_TX_BUFSIZE     1024
#endif
