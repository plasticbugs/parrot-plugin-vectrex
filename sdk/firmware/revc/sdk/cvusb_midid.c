// cvusb_midid.c -- cvusb's MIDI_OUT: the cart's USB port as a MIDI device,
// so that with a computer on the port (the computer the host) what the
// plugin sends plays on a synth there, and what the computer sends comes in.
//
// The SCUMM plugin's usbmidi_dev.c (../scumm/plugin), which plays Monkey
// Island's MT-32 music on Munt this way, with the plugin's own name on the
// device, and what the computer sends handed to cvusb.c instead of dropped.
// TinyUSB's device stack and its MIDI class, one cable; all of it from the
// plugin's loop (cvusb_task()).
//
// The device's names and IDs: CVUSB_DEVICE_NAME is the plugin's NAME
// (cvcart_plugin() sets it), what the computer lists the MIDI port as.
// CVUSB_VID and CVUSB_PID are TinyUSB's examples' vendor ID and a product
// ID of the cart's: fine for a plugin people use at home. Set your own
// (target_compile_definitions) if you have one (https://pid.codes gives
// IDs to open-source projects).
#include <string.h>

#include "hardware/structs/usb.h"
#include "hardware/sync.h"
#include "tusb.h"

#include "cvusb_pvt.h"

#ifndef CVUSB_DEVICE_NAME
#define CVUSB_DEVICE_NAME "Parrot Cart MIDI"
#endif
#ifndef CVUSB_VID
#define CVUSB_VID 0xCAFE            // TinyUSB's examples' vendor
#endif
#ifndef CVUSB_PID
#define CVUSB_PID 0x4C13            // the cart's plugins (SCUMM is 4C11, the kit's MIDI example 4C12)
#endif

// MIDI bytes waiting to go out (a power of 2). A plugin that sends big
// SysEx at once wants more: the SCUMM plugin's 16K holds Monkey Island's
// MT-32 set-up (~5 KB).
#ifndef CVUSB_MIDI_OUT_QUEUE
#define CVUSB_MIDI_OUT_QUEUE 4096
#endif
#define QBYTES CVUSB_MIDI_OUT_QUEUE
_Static_assert((QBYTES & (QBYTES - 1)) == 0, "CVUSB_MIDI_OUT_QUEUE: a power of 2");

static uint8_t q[QBYTES];
static uint32_t q_head, q_tail;     // free-running: q_tail - q_head bytes queued
static uint32_t sof0;               // the frame number at the start: a host's frames move it
static bool host_seen;

// ---- descriptors ---------------------------------------------------------------------

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,              // each interface says
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CVUSB_VID,
    .idProduct = CVUSB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

enum { ITF_MIDI = 0, ITF_MIDI_STREAMING, ITF_TOTAL };
#define EP_OUT 0x01
#define EP_IN  0x81
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_MIDI, 4, EP_OUT, EP_IN, 64),
};

static const char *const strings[] = {
    NULL,                           // 0: the language, below
    "Parrot Cart",
    CVUSB_DEVICE_NAME,
    "1",
    CVUSB_DEVICE_NAME,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_config;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t d[32];
    unsigned n;
    if (index == 0) {
        d[1] = 0x0409;              // English (US)
        n = 1;
    } else {
        if (index >= sizeof strings / sizeof strings[0]) return NULL;
        const char *s = strings[index];
        n = (unsigned)strlen(s);
        if (n > 31) n = 31;
        for (unsigned i = 0; i < n; i++) d[1 + i] = (uint8_t)s[i];
    }
    d[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * n + 2));
    return d;
}

// ---- the port ------------------------------------------------------------------------

void cvmidid_start(void) {
    q_head = q_tail = 0;
    tusb_rhport_init_t dev = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    tusb_init(0, &dev);
    sof0 = usb_hw->sof_rd & USB_SOF_RD_BITS;
    host_seen = false;
}

// A computer is there: its frames (one a millisecond, each with the next
// frame number) or a request, whether or not it has taken the device yet.
bool cvmidid_host_seen(void) {
    if (!host_seen && tud_inited() && (tud_connected() || (usb_hw->sof_rd & USB_SOF_RD_BITS) != sof0)) host_seen = true;
    return host_seen;
}

bool cvmidid_mounted(void) { return tud_inited() && tud_mounted(); }

// The port let go of, for the host to have it: as the menu does
// (../sdplayer/usbrole.c), with interrupts off.
void cvmidid_stop(void) {
    if (!tud_inited()) return;
    const uint32_t irq = save_and_disable_interrupts();
    tusb_deinit(0);
    restore_interrupts(irq);
}

bool cvmidid_write(const uint8_t *msg, uint32_t n) {
    if (n == 0) return true;
    if (QBYTES - (q_tail - q_head) < n) return false;
    for (uint32_t i = 0; i < n; i++) q[(q_tail + i) & (QBYTES - 1)] = msg[i];
    q_tail += n;
    return true;
}

void cvmidid_task(void) {
    if (!tud_inited()) return;
    tud_task();
    // what the computer sends
    uint8_t pkt[4];
    while (tud_midi_available() && tud_midi_packet_read(pkt)) cvusb_midi_packet(pkt);
    if (!tud_mounted()) return;
    // the queue into TinyUSB's FIFO: it packs the bytes into USB-MIDI packets
    // (a SysEx across several) and takes as many as it has room for
    while (q_tail != q_head) {
        uint32_t at = q_head & (QBYTES - 1), n = q_tail - q_head;
        if (n > QBYTES - at) n = QBYTES - at;
        uint32_t took = tud_midi_stream_write(0, q + at, n);
        if (!took) break;
        q_head += took;
    }
}
