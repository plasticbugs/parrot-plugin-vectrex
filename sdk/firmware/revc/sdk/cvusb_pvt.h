// cvusb_pvt.h -- between cvusb.c and its parts (cvpad.c, cvusb_midih.c,
// cvusb_midid.c). Not for plugins: cvusb.h is theirs.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// cvusb.c: a USB-MIDI packet that came in (from either side)
void cvusb_midi_packet(const uint8_t pkt[4]);

// cvpad.c built with CVUSB: the pads' share of TinyUSB's HID callbacks, and
// of the task
void cvpad_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const *desc, uint16_t len);
void cvpad_hid_umount_cb(uint8_t daddr, uint8_t idx);
void cvpad_hid_report_received_cb(uint8_t daddr, uint8_t idx, uint8_t const *r, uint16_t len);
void cvpad_poll(uint32_t now_us);

// cvusb_midih.c (MIDI_IN): the host's MIDI device. In cvusb's task, but
// room() and send(), from the loop with interrupts off. room(): packets
// that fit in the queue now, -1 if there's no device to send to.
void cvmidih_task(void);
int cvmidih_room(void);
void cvmidih_send(const uint8_t pkt[4]);
bool cvmidih_mounted(void);

// cvusb_midid.c (MIDI_OUT): the cart as a MIDI device, all from the loop
void cvmidid_start(void);
void cvmidid_stop(void);
void cvmidid_task(void);                // TinyUSB, the queue out, and what came in to cvusb_midi_packet()
bool cvmidid_mounted(void);             // a computer has taken the device
bool cvmidid_host_seen(void);           // a computer has answered
bool cvmidid_write(const uint8_t *msg, uint32_t n);
