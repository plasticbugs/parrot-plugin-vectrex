// cvusb_midi.c -- see cvusb_midi.h.
#include "cvusb_midi.h"

enum { DESC_INTERFACE = 4, DESC_ENDPOINT = 5, CLASS_AUDIO = 1, SUBCLASS_MIDISTREAMING = 3 };

bool cvmidi_find(const uint8_t *p, uint32_t len, cvmidi_found_t *out) {
    const uint8_t *start = p, *end = p + len;
    bool in_ms = false;
    out->ep_in = out->ep_out = 0;
    out->len = 0;
    while (end - p >= 2 && p[0] >= 2 && p[0] <= end - p) {
        if (p[1] == DESC_INTERFACE && p[0] >= 9) {
            if (out->ep_in) break;                              // that MIDIStreaming interface is done
            in_ms = p[5] == CLASS_AUDIO && p[6] == SUBCLASS_MIDISTREAMING;
            if (in_ms) out->ms_itf = p[2];
        } else if (p[1] == DESC_ENDPOINT && p[0] >= 7 && in_ms && (p[3] & 3) >= 2) {   // bulk or interrupt
            if (p[2] & 0x80) {
                if (!out->ep_in) out->ep_in = p;
            } else if (!out->ep_out) {
                out->ep_out = p;
            }
        }
        p += p[0];
    }
    if (!out->ep_in) return false;
    out->len = (uint16_t)(p - start);       // to the next interface, or as far as the walk could go
    return true;
}

// the bytes in a channel message, by its status's top nibble (8-E)
static int channel_len(uint8_t status) { return (status >> 4) == 0xC || (status >> 4) == 0xD ? 2 : 3; }

bool cvmidi_channel_msg(const uint8_t pkt[4], uint8_t msg[3]) {
    uint8_t cin = pkt[0] & 0x0F;
    // CIN 8-E are the channel messages, and they must agree with the status byte
    if (cin < 0x8 || cin > 0xE || pkt[1] >> 4 != cin) return false;
    msg[0] = pkt[1];
    msg[1] = pkt[2] & 0x7F;
    msg[2] = (cin == 0xC || cin == 0xD) ? 0 : (pkt[3] & 0x7F);
    return true;
}

int cvmidi_msg(const uint8_t pkt[4], uint8_t msg[3]) {
    const uint8_t cin = pkt[0] & 0x0F, s = pkt[1];
    msg[0] = s;
    msg[1] = msg[2] = 0;
    if (cin >= 0x8 && cin <= 0xE) {
        if (!cvmidi_channel_msg(pkt, msg)) return 0;
        return channel_len(s);
    }
    switch (cin) {
    case 0x2:                                   // two bytes of system common: MTC quarter frame, song select
        if (s != 0xF1 && s != 0xF3) return 0;
        msg[1] = pkt[2] & 0x7F;
        return 2;
    case 0x3:                                   // three: song position
        if (s != 0xF2) return 0;
        msg[1] = pkt[2] & 0x7F;
        msg[2] = pkt[3] & 0x7F;
        return 3;
    case 0x5:                                   // one byte of system common (tune request), or a SysEx's end
        return s == 0xF6 ? 1 : 0;
    case 0xF:                                   // one byte: real time (clock, start, stop...)
        return s >= 0xF8 ? 1 : 0;
    default:                                    // SysEx (4, 6, 7), reserved (0, 1)
        return 0;
    }
}

// ---- the way back -------------------------------------------------------------------
// a whole message's length by its status (0: not one we send)
static int msg_len(uint8_t s) {
    if (s >= 0x80 && s < 0xF0) return channel_len(s);
    switch (s) {
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    case 0xF6: return 1;
    default: return 0;                          // F4, F5 (undefined), F7 alone
    }
}

static bool put(uint8_t (*pkts)[4], int max, int *k, uint8_t cin, const uint8_t *m) {
    if (*k >= max) return false;
    pkts[*k][0] = cin;                          // cable 0
    pkts[*k][1] = m[0];
    pkts[*k][2] = m[1];
    pkts[*k][3] = m[2];
    (*k)++;
    return true;
}

int cvmidi_pack(const uint8_t *b, uint32_t n, uint8_t (*pkts)[4], int max, uint8_t *status) {
    int k = 0;
    uint8_t m[3] = {0}, have = 0, need = 0;     // a message being gathered
    bool sysex = false;                         // m holds up to 3 bytes of a SysEx
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = b[i];
        if (c >= 0xF8) {                        // real time: on its own, even inside a SysEx
            const uint8_t one[3] = {c, 0, 0};
            if (!put(pkts, max, &k, 0xF, one)) return -1;
            continue;
        }
        if (sysex) {
            if (c == 0xF7 || c < 0x80) {
                m[have++] = c;
                if (c == 0xF7) {                // the end: 1, 2 or 3 bytes in its packet
                    if (!put(pkts, max, &k, (uint8_t)(0x4 + have), m)) return -1;
                    sysex = false;
                    have = 0;
                    m[0] = m[1] = m[2] = 0;
                } else if (have == 3) {
                    if (!put(pkts, max, &k, 0x4, m)) return -1;
                    have = 0;
                    m[0] = m[1] = m[2] = 0;
                }
                continue;
            }
            sysex = false;                      // a status byte cuts it short: dropped
            have = 0;
            m[0] = m[1] = m[2] = 0;
        }
        if (c == 0xF0) {
            sysex = true;
            m[0] = c;
            have = 1;
            need = 0;
            *status = 0;
            continue;
        }
        if (c >= 0x80) {                        // a status: a new message
            need = (uint8_t)msg_len(c);
            *status = c < 0xF0 ? c : 0;         // system common ends running status
            m[0] = c;
            m[1] = m[2] = 0;
            have = need ? 1 : 0;
        } else if (have) {
            m[have++] = c;
        } else if (*status) {                   // running status: the last channel status again
            need = (uint8_t)msg_len(*status);
            m[0] = *status;
            m[1] = c;
            m[2] = 0;
            have = 2;
        }                                       // (else a stray data byte: dropped)
        if (have && have == need) {
            const uint8_t cin = m[0] < 0xF0 ? (uint8_t)(m[0] >> 4) : need == 1 ? 0x5 : need;   // F6: 5; F1, F3: 2; F2: 3
            if (!put(pkts, max, &k, cin, m)) return -1;
            have = 0;
            m[0] = m[1] = m[2] = 0;
        }
    }
    return k;
}
