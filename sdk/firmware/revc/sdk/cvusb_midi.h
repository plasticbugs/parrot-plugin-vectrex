// cvusb_midi.h -- USB MIDI 1.0 with no USB stack, for cvusb.c: finding a
// device's MIDI endpoints in its configuration descriptor, and turning the
// 4-byte event packets USB carries MIDI in into MIDI messages and back.
//
// From PSG Synth's usbmidi.c (../midihello), which finds a USB MIDI
// keyboard's endpoints the same way: usbmidi_find() and
// usbmidi_channel_msg() are its own, with how far the MIDIStreaming
// interface's descriptors go (cvmidi_found_t.len: TinyUSB 0.21's drivers
// say that), every kind of message rather than the channel ones, and the
// way back (cvmidi_pack()).
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t ms_itf;             // the MIDIStreaming interface's number
    const uint8_t *ep_in;       // its first IN endpoint descriptor (bulk, or interrupt)
    const uint8_t *ep_out;      // its first OUT endpoint descriptor, or NULL
    uint16_t len;               // bytes from the start to the end of that interface's descriptors
} cvmidi_found_t;

// Walk len bytes of descriptors from an interface descriptor of class Audio
// (Audio Control, or MIDIStreaming itself) and find the MIDIStreaming
// interface and its first IN and OUT endpoints. True if it has an IN.
bool cvmidi_find(const uint8_t *desc, uint32_t len, cvmidi_found_t *out);

// A USB-MIDI event packet (cable and code index, then up to 3 MIDI bytes)
// -> its message in msg, status first; returns its length, 1 to 3. 0 for
// a SysEx's packets (not passed on), padding, and packets that don't agree
// with themselves.
int cvmidi_msg(const uint8_t pkt[4], uint8_t msg[3]);

// A channel message only (note, pressure, control, program, bend), as PSG
// Synth's usbmidi_channel_msg(): true and msg filled in.
bool cvmidi_channel_msg(const uint8_t pkt[4], uint8_t msg[3]);

// MIDI bytes -> USB-MIDI packets, cable 0. The bytes are whole messages
// (running status is fine: the last status is kept in *status), a SysEx
// from F0 to F7 included. Up to max packets are written to pkts; returns
// how many, or -1 if they don't fit (nothing is then sent: pkts is
// scratch). Bytes that aren't MIDI (a data byte with no status before it)
// are dropped.
int cvmidi_pack(const uint8_t *bytes, uint32_t n, uint8_t (*pkts)[4], int max, uint8_t *status);

#ifdef __cplusplus
}
#endif
