/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if (CFG_TUH_ENABLED && CFG_TUH_HUB)

#include "hcd.h"
#include "usbh.h"
#include "usbh_pvt.h"
#include "hub.h"

// Debug level, TUSB_CFG_DEBUG must be at least this level for debug message
#define HUB_DEBUG   2
#define TU_LOG_DRV(...)   TU_LOG(HUB_DEBUG, __VA_ARGS__)

// consecutive failed status-change polls before the hub or a port status is read directly
#define HUB_POLL_FAIL_THRESHOLD  3

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+
typedef struct {
  uint8_t itf_num;
  uint8_t ep_in;

  // from hub descriptor
  uint8_t bNbrPorts;
  uint8_t bPwrOn2PwrGood_2ms; // port power on to good, in 2ms unit
  // uint16_t wHubCharacteristics;
  bool mtt;

  uint8_t poll_failed_count; // consecutive failed status-change interrupt polls
  uint8_t recover_port;      // last status read by the recovery: 0 = hub, 1..bNbrPorts = port
  uint16_t present;          // cvcart: ports (bit n = port n) found with a device at power-up and no change flagged
  bool     ready;            // cvcart: configured: hub_poll() reads its ports
  bool     busy;             // cvcart: a port's status change being read and handled (hub_poll() waits)
  uint8_t  poll_port;        // cvcart: the port hub_poll() read last
  uint32_t poll_at_ms;       // cvcart: when it reads the next
  uint32_t busy_ms;          // cvcart: when busy was set
} hub_interface_t;

// cvcart: hub_poll() reads one port's status this often. A Genesys GL3523 (Anker) never answers its status
// endpoint, so a device plugged into it, or pulled out, is seen only this way.
#define HUB_POLL_MS 100

typedef struct {
  TUH_EPBUF_DEF(status_change, 4); // interrupt endpoint
  TUH_EPBUF_DEF(ctrl_buf, CFG_TUH_HUB_BUFSIZE);
} hub_epbuf_t;

static hub_interface_t hub_itfs[CFG_TUH_HUB];
CFG_TUH_MEM_SECTION static hub_epbuf_t hub_epbufs[CFG_TUH_HUB];


TU_ATTR_ALWAYS_INLINE static inline hub_interface_t* get_hub_itf(uint8_t daddr) {
  return &hub_itfs[daddr-1-CFG_TUH_DEVICE_MAX];
}

TU_ATTR_ALWAYS_INLINE static inline hub_epbuf_t* get_hub_epbuf(uint8_t daddr) {
  return &hub_epbufs[daddr-1-CFG_TUH_DEVICE_MAX];
}

// last port GET_STATUS response, kept in ctrl_buf until the next data-stage request to this hub
TU_ATTR_ALWAYS_INLINE static inline const hub_port_status_response_t* get_port_status(uint8_t daddr) {
  return (const hub_port_status_response_t*) (uintptr_t) get_hub_epbuf(daddr)->ctrl_buf;
}

#if CFG_TUSB_DEBUG >= HUB_DEBUG
static char const* const _hub_feature_str[] = {
  [HUB_FEATURE_PORT_CONNECTION          ] = "PORT_CONNECTION",
  [HUB_FEATURE_PORT_ENABLE              ] = "PORT_ENABLE",
  [HUB_FEATURE_PORT_SUSPEND             ] = "PORT_SUSPEND",
  [HUB_FEATURE_PORT_OVER_CURRENT        ] = "PORT_OVER_CURRENT",
  [HUB_FEATURE_PORT_RESET               ] = "PORT_RESET",
  [HUB_FEATURE_PORT_POWER               ] = "PORT_POWER",
  [HUB_FEATURE_PORT_LOW_SPEED           ] = "PORT_LOW_SPEED",
  [HUB_FEATURE_PORT_CONNECTION_CHANGE   ] = "PORT_CONNECTION_CHANGE",
  [HUB_FEATURE_PORT_ENABLE_CHANGE       ] = "PORT_ENABLE_CHANGE",
  [HUB_FEATURE_PORT_SUSPEND_CHANGE      ] = "PORT_SUSPEND_CHANGE",
  [HUB_FEATURE_PORT_OVER_CURRENT_CHANGE ] = "PORT_OVER_CURRENT_CHANGE",
  [HUB_FEATURE_PORT_RESET_CHANGE        ] = "PORT_RESET_CHANGE",
  [HUB_FEATURE_PORT_TEST                ] = "PORT_TEST",
  [HUB_FEATURE_PORT_INDICATOR           ] = "PORT_INDICATOR",
};
#endif

//--------------------------------------------------------------------+
// HUB
//--------------------------------------------------------------------+
bool hub_port_clear_feature(uint8_t hub_addr, uint8_t hub_port, uint8_t feature,
                            tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  tusb_control_request_t const request = {
    .bmRequestType_bit = {
      .recipient = (hub_port == 0) ? TUSB_REQ_RCPT_DEVICE : TUSB_REQ_RCPT_OTHER,
      .type      = TUSB_REQ_TYPE_CLASS,
      .direction = TUSB_DIR_OUT
    },
    .bRequest = HUB_REQUEST_CLEAR_FEATURE,
    .wValue   = feature,
    .wIndex   = hub_port,
    .wLength  = 0
  };

  tuh_xfer_t xfer = {
    .daddr       = hub_addr,
    .ep_addr     = 0,
    .setup       = &request,
    .buffer      = NULL,
    .complete_cb = complete_cb,
    .user_data   = user_data
  };

  TU_LOG_DRV("HUB Clear Feature: %s, addr = %u port = %u\r\n", _hub_feature_str[feature], hub_addr, hub_port);
  TU_ASSERT(tuh_control_xfer(&xfer));
  return true;
}

bool hub_port_set_feature(uint8_t hub_addr, uint8_t hub_port, uint8_t feature,
                          tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  tusb_control_request_t const request = {
    .bmRequestType_bit = {
      .recipient = (hub_port == 0) ? TUSB_REQ_RCPT_DEVICE : TUSB_REQ_RCPT_OTHER,
      .type      = TUSB_REQ_TYPE_CLASS,
      .direction = TUSB_DIR_OUT
    },
    .bRequest = HUB_REQUEST_SET_FEATURE,
    .wValue   = feature,
    .wIndex   = hub_port,
    .wLength  = 0
  };

  tuh_xfer_t xfer = {
    .daddr       = hub_addr,
    .ep_addr     = 0,
    .setup       = &request,
    .buffer      = NULL,
    .complete_cb = complete_cb,
    .user_data   = user_data
  };

  TU_LOG_DRV("HUB Set Feature: %s, addr = %u port = %u\r\n", _hub_feature_str[feature], hub_addr, hub_port);
  TU_ASSERT(tuh_control_xfer(&xfer));
  return true;
}

bool hub_port_get_status(uint8_t hub_addr, uint8_t hub_port, void* resp,
                         tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  tusb_control_request_t const request = {
    .bmRequestType_bit = {
      .recipient = (hub_port == 0) ? TUSB_REQ_RCPT_DEVICE : TUSB_REQ_RCPT_OTHER,
      .type      = TUSB_REQ_TYPE_CLASS,
      .direction = TUSB_DIR_IN
    },
    .bRequest = HUB_REQUEST_GET_STATUS,
    .wValue   = 0,
    .wIndex   = hub_port,
    .wLength  = tu_htole16(4)
  };

  tuh_xfer_t xfer = {
    .daddr       = hub_addr,
    .ep_addr     = 0,
    .setup       = &request,
    .buffer      = (hub_port != 0) ? get_hub_epbuf(hub_addr)->ctrl_buf : resp,
    .complete_cb = complete_cb,
    .user_data   = user_data
  };

  TU_LOG_DRV("HUB Get Port Status: addr = %u port = %u\r\n", hub_addr, hub_port);
  TU_VERIFY(tuh_control_xfer(&xfer));
  return true;
}

bool hub_port_get_status_local(uint8_t hub_addr, uint8_t hub_port, hub_port_status_response_t* resp) {
  (void) hub_port;
  *resp = *get_port_status(hub_addr);
  return true;
}

//--------------------------------------------------------------------+
// CLASS-USBH API (don't require to verify parameters)
//--------------------------------------------------------------------+
bool hub_init(void) {
  tu_memclr(hub_itfs, sizeof(hub_itfs));
  return true;
}

bool hub_deinit(void) {
  return true;
}

uint16_t hub_open(uint8_t rhport, uint8_t dev_addr, const tusb_desc_interface_t *itf_desc, uint16_t max_len) {
  (void)rhport;
  TU_VERIFY(dev_addr > CFG_TUH_DEVICE_MAX, 0); // only a hub-class device gets a hub address
  TU_VERIFY(TUSB_CLASS_HUB == itf_desc->bInterfaceClass && 0 == itf_desc->bInterfaceSubClass, 0);

  const uint16_t itf_ep_len = sizeof(tusb_desc_interface_t) + sizeof(tusb_desc_endpoint_t);
  uint16_t       drv_len    = itf_ep_len;
  TU_ASSERT(drv_len <= max_len, 0);

  hub_interface_t             *p_hub        = get_hub_itf(dev_addr);
  const tusb_desc_interface_t *desc_itf_use = itf_desc; // interface to use for endpoint open

  // Check device descriptor for MTT hub (bDeviceProtocol == 2)
  // MTT hub has 2 alt settings: alt 0 is STT (protocol 1), alt 1 is MTT (protocol 2)
  // Consume both alt settings and use alt setting 1 for endpoint
  bool               mtt = false;
  tusb_desc_device_t desc_dev;
  if (tuh_descriptor_get_device_local(dev_addr, &desc_dev) && desc_dev.bDeviceProtocol == HUB_PROTOCOL_HIGH_SPEED_MTT) {
    drv_len += itf_ep_len;
    TU_ASSERT(drv_len <= max_len, 0);
    const tusb_desc_interface_t *desc_alt1 = (const tusb_desc_interface_t *)((const uint8_t *)itf_desc + itf_ep_len);
    TU_ASSERT(desc_alt1->bDescriptorType == TUSB_DESC_INTERFACE && desc_alt1->bInterfaceClass == TUSB_CLASS_HUB &&
                desc_alt1->bInterfaceProtocol == HUB_PROTOCOL_HIGH_SPEED_MTT,
              0);
    mtt          = true;
    desc_itf_use = desc_alt1;
  }

  // Interrupt Status endpoint
  const tusb_desc_endpoint_t *desc_ep = (const tusb_desc_endpoint_t *)tu_desc_next(desc_itf_use);
  TU_ASSERT(TUSB_DESC_ENDPOINT == desc_ep->bDescriptorType && TUSB_XFER_INTERRUPT == desc_ep->bmAttributes.xfer, 0);
  TU_ASSERT(tuh_edpt_open(dev_addr, desc_ep), 0);

  // set only once opened: hub_close() clears an opened hub
  p_hub->mtt     = mtt;
  p_hub->itf_num = itf_desc->bInterfaceNumber;
  p_hub->ep_in   = desc_ep->bEndpointAddress;

  return drv_len;
}

void hub_close(uint8_t dev_addr) {
  TU_VERIFY(dev_addr > CFG_TUH_DEVICE_MAX, );
  hub_interface_t* p_hub = get_hub_itf(dev_addr);

  if (p_hub->ep_in) {
    TU_LOG_DRV("  HUB close addr = %d\r\n", dev_addr);
    tu_memclr(p_hub, sizeof( hub_interface_t));
  }
}

// cvcart: what the status endpoint and hub_poll() see, for a diagnostic screen (Pad Test). Weak, does nothing by
// default. what 1: the status endpoint answered (value = the result << 8 | its change bitmap); 2: a poll found a
// change (value = the port << 16 | the port's change bits); 3: the status endpoint could not be armed; 4: a change
// whose handling was dropped (its device gone part-way) was given up after a second.
TU_ATTR_WEAK void tuh_hub_diag_cb(uint8_t daddr, uint8_t what, uint32_t value) {
  (void) daddr; (void) what; (void) value;
}

// cvcart: the status endpoint armed, unless it already is: a change hub_poll() found ends with usbh or
// process_new_status() asking for it again, while it may still be waiting from before
static bool status_arm(uint8_t daddr) {
  hub_interface_t* p_hub = get_hub_itf(daddr);
  hub_epbuf_t* p_epbuf = get_hub_epbuf(daddr);

  TU_VERIFY(p_hub->ep_in != 0); // closed
  if (usbh_edpt_busy(daddr, p_hub->ep_in)) {
    return true;
  }
  if (!usbh_edpt_claim(daddr, p_hub->ep_in)) {
    tuh_hub_diag_cb(daddr, 3, 0);
    return false;
  }
  if (!usbh_edpt_xfer(daddr, p_hub->ep_in, p_epbuf->status_change, 1)) {
    usbh_edpt_release(daddr, p_hub->ep_in);
    tuh_hub_diag_cb(daddr, 3, 1);
    return false;
  }

  return true;
}

// the end of a status change's handling, here or in usbh after an attached device's enumeration (cvcart: so
// hub_poll() goes on too)
bool hub_edpt_status_xfer(uint8_t daddr) {
  get_hub_itf(daddr)->busy = false;
  return status_arm(daddr);
}

//--------------------------------------------------------------------+
// Set Configure
//--------------------------------------------------------------------+
static void config_set_port_power (tuh_xfer_t* xfer);
static void config_port_power_complete (tuh_xfer_t* xfer);
static void config_scan_start (uintptr_t param);
static void config_scan_port (tuh_xfer_t* xfer);
static void config_scan_cleared (tuh_xfer_t* xfer);
static void config_scan_next (uint8_t daddr, uint8_t port);
static void config_complete (uint8_t daddr);
static void config_get_hub_descriptor(tuh_xfer_t* xfer);

bool hub_set_config(uint8_t daddr, uint8_t itf_num) {
  hub_interface_t* p_hub = get_hub_itf(daddr);
  TU_ASSERT(itf_num == p_hub->itf_num);

  if (p_hub->mtt) {
    // Set Alternate Setting 1 for MTT hub
    TU_ASSERT(tuh_interface_set(daddr, itf_num, 1, config_get_hub_descriptor, 0));
  } else {
    tuh_xfer_t xfer;
    xfer.daddr     = daddr;
    xfer.ep_addr   = 0;
    xfer.result    = XFER_RESULT_SUCCESS;
    xfer.user_data = 0;
    config_get_hub_descriptor(&xfer);
  }

  return true;
}

static void config_get_hub_descriptor(tuh_xfer_t* xfer) {
  if (xfer->result != XFER_RESULT_SUCCESS) {
    // A STALL keeps alternate setting 0, single-TT (USB 2.0 9.4.10, 11.23.1); after a transport error the setting is
    // unknown. Either way schedule it as single-TT: one TT budget for every port is safe for both organizations.
    get_hub_itf(xfer->daddr)->mtt = false;
  }

  // Get Hub Descriptor
  tusb_control_request_t const request = {
    .bmRequestType_bit = {
      .recipient = TUSB_REQ_RCPT_DEVICE,
      .type      = TUSB_REQ_TYPE_CLASS,
      .direction = TUSB_DIR_IN
    },
    .bRequest = HUB_REQUEST_GET_DESCRIPTOR,
    .wValue   = 0x29 << 8, // cvcart: the hub descriptor type (USB 2.0 11.24.2.5); 0 makes a VIA VL817 stall
    .wIndex   = 0,
    .wLength  = sizeof(hub_desc_cs_t)
  };

  xfer->setup       = &request;
  xfer->buffer      = get_hub_epbuf(xfer->daddr)->ctrl_buf;
  xfer->complete_cb = config_set_port_power;

  TU_ASSERT(tuh_control_xfer(xfer), );
}

static void config_set_port_power (tuh_xfer_t* xfer) {
  TU_ASSERT(XFER_RESULT_SUCCESS == xfer->result, );

  uint8_t const daddr = xfer->daddr;
  hub_interface_t* p_hub = get_hub_itf(daddr);
  hub_epbuf_t* p_epbuf = get_hub_epbuf(daddr);

  // only use the number of ports in the hub descriptor
  hub_desc_cs_t const* desc_hub = (hub_desc_cs_t const*) p_epbuf->ctrl_buf;
  p_hub->bNbrPorts = desc_hub->bNbrPorts;
  p_hub->bPwrOn2PwrGood_2ms = desc_hub->bPwrOn2PwrGood;

  // May need to GET_STATUS

  // Set Port Power to be able to detect connection, starting with port 1
  uint8_t const hub_port = 1;
  hub_port_set_feature(daddr, hub_port, HUB_FEATURE_PORT_POWER, config_port_power_complete, 0);
}

static void config_port_power_complete (tuh_xfer_t* xfer) {
  TU_ASSERT(XFER_RESULT_SUCCESS == xfer->result, );

  uint8_t const daddr = xfer->daddr;
  hub_interface_t* p_hub = get_hub_itf(daddr);

  if (xfer->setup->wIndex == p_hub->bNbrPorts) {
    // All ports are power -> queue notification status endpoint and
    // complete the SET CONFIGURATION
    // cvcart: then read every port's status once (config_scan_port). A hub that was already powered, by its own
    // charger, with a device on a port (a Genesys GL3523 does this) never names that port on its status endpoint, so
    // the device is never seen. Linux and macOS read every port at a hub's start for the same reason, after the
    // ports' power has had its bPwrOn2PwrGood time (at least 100 ms), and take any port with a device on it as new
    // whatever its change flags say. So does this. It runs inside the hub's own enumeration, so no other control
    // transfer can be under way. (Pad Test 0.1.3 read the ports straight after powering them and left a port whose
    // connection change was flagged to the status endpoint: the pad was still missed.)
    p_hub->present = 0;
    uint32_t wait_ms = 2u * p_hub->bPwrOn2PwrGood_2ms;
    if (wait_ms < 100) wait_ms = 100;
    tuh_hub_scan_cb(daddr, 0, p_hub->bNbrPorts, (uint16_t) wait_ms);
    if (!usbh_defer_func_ms_async(wait_ms, config_scan_start, daddr)) {
      config_scan_start(daddr);     // (the deferred call's one slot was taken: read them now)
    }
  } else {
    // power next port
    uint8_t const hub_port = (uint8_t) (xfer->setup->wIndex + 1);
    hub_port_set_feature(daddr, hub_port, HUB_FEATURE_PORT_POWER, config_port_power_complete, 0);
  }
}

// cvcart: what the scan found, for a diagnostic screen (Pad Test). port 0: the scan is about to start, status = the
// number of ports, change = the wait in ms. Otherwise a port's status and change bits as read.
TU_ATTR_WEAK void tuh_hub_scan_cb(uint8_t daddr, uint8_t port, uint16_t status, uint16_t change) {
  (void) daddr; (void) port; (void) status; (void) change;
}

// cvcart: the ports' power has settled: read port 1 (then each in turn)
static void config_scan_start (uintptr_t param) {
  uint8_t const daddr = (uint8_t) param;
  if (!tuh_connected(daddr)) {
    return;                         // the hub went while its ports' power settled: its removal ends the enumeration
  }
  if (!hub_port_get_status(daddr, 1, NULL, config_scan_port, 0)) {
    config_complete(daddr);
  }
}

// cvcart: one port's status read at power-up. A device there is taken as new, as Linux does; a connection change
// flagged on the port is cleared first, so the status endpoint doesn't name it again and start the device over.
static void config_scan_port (tuh_xfer_t* xfer) {
  uint8_t const daddr = xfer->daddr;
  hub_interface_t* p_hub = get_hub_itf(daddr);
  uint8_t const port = (uint8_t) tu_le16toh(xfer->setup->wIndex);

  if (xfer->result == XFER_RESULT_SUCCESS && xfer->actual_len >= sizeof(hub_port_status_response_t) && port < 16) {
    hub_port_status_response_t const* st = (hub_port_status_response_t const*) get_hub_epbuf(daddr)->ctrl_buf;
    TU_LOG_DRV("HUB port %u at power-up: status %04x change %04x\r\n", port, st->status.value, st->change.value);
    tuh_hub_scan_cb(daddr, port, st->status.value, st->change.value);
    if (st->status.connection) {
      p_hub->present |= (uint16_t) (1u << port);
    }
    if (st->change.connection &&
        hub_port_clear_feature(daddr, port, HUB_FEATURE_PORT_CONNECTION_CHANGE, config_scan_cleared, 0)) {
      return;
    }
  } else {
    tuh_hub_scan_cb(daddr, port, 0xFFFF, 0xFFFF);      // (no answer)
  }
  config_scan_next(daddr, port);
}

static void config_scan_cleared (tuh_xfer_t* xfer) {
  config_scan_next(xfer->daddr, (uint8_t) tu_le16toh(xfer->setup->wIndex));
}

// cvcart: the next port, or the end of the configuration
static void config_scan_next (uint8_t daddr, uint8_t port) {
  hub_interface_t* p_hub = get_hub_itf(daddr);
  if (port < p_hub->bNbrPorts && hub_port_get_status(daddr, (uint8_t) (port + 1), NULL, config_scan_port, 0)) {
    return;
  }
  config_complete(daddr);
}

// cvcart: the end of the hub's configuration. With devices found at power-up, each is attached as if its port had
// been flagged, and the status endpoint is left unpolled as after STATE_CHECK_CONN: usbh polls it again during the
// first device's enumeration (or when that fails), and enumerates the others in turn from its deferred attaches.
static void config_complete (uint8_t daddr) {
  hub_interface_t* p_hub = get_hub_itf(daddr);
  uint16_t const present = p_hub->present;
  p_hub->present = 0;

  if (!present && !hub_edpt_status_xfer(daddr)) {
    TU_MESS_FAILED();
    TU_BREAKPOINT();
  }
  p_hub->ready      = true;
  p_hub->poll_at_ms = tusb_time_millis_api() + HUB_POLL_MS;
  usbh_driver_set_config_complete(daddr, p_hub->itf_num);

  for (uint8_t port = 1; port < 16; port++) {
    if (present & (1u << port)) {
      TU_LOG_DRV("HUB port %u: a device already there at power-up\r\n", port);
      const hcd_event_t event = {
        .rhport     = usbh_get_rhport(daddr),
        .event_id   = HCD_EVENT_DEVICE_ATTACH,
        .connection = {
          .hub_addr = daddr,
          .hub_port = port
        }
      };
      hcd_event_handler(&event, false);
    }
  }
}

//--------------------------------------------------------------------+
// Connection Changes
//--------------------------------------------------------------------+
enum {
  STATE_IDLE = 0,
  STATE_HUB_STATUS,
  STATE_CLEAR_CHANGE,
  STATE_CHECK_CONN,
  STATE_COMPLETE
};

static void process_new_status(tuh_xfer_t* xfer);

// callback as response of interrupt endpoint polling
bool hub_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  (void) xferred_bytes;
  (void) ep_addr;

  bool processed = false; // true if new status is processed
  hub_interface_t* p_hub = get_hub_itf(daddr);

  tuh_hub_diag_cb(daddr, 1, (uint32_t) result << 8 | get_hub_epbuf(daddr)->status_change[0]);
  if (p_hub->busy) {
    // cvcart: hub_poll() is reading a port, in ctrl_buf: the hub keeps a change until it is cleared, and names it
    // again, or the next poll finds it
    TU_ASSERT(status_arm(daddr));
    return true;
  }

  if (result == XFER_RESULT_SUCCESS) {
    p_hub->poll_failed_count = 0;
    hub_epbuf_t *p_epbuf = get_hub_epbuf(daddr);
    const uint8_t status_change = p_epbuf->status_change[0];
    TU_LOG_DRV("  Hub Status Change = 0x%02X\r\n", status_change);

    if (status_change == 0) {
      // The status change event was neither for the hub, nor for any of its ports.
      // This shouldn't happen, but it does with some devices. Re-Initiate the interrupt poll.
      processed = false;
    } else if (tu_bit_test(status_change, 0)) {
      // Hub bit 0 is for the hub device events
      processed = hub_get_status(daddr, p_epbuf->ctrl_buf, process_new_status, STATE_HUB_STATUS);
    } else {
      // Hub bits 1 to n are hub port events
      for (uint8_t port=1; port <= p_hub->bNbrPorts; port++) {
        if (tu_bit_test(status_change, port)) {
          processed = hub_port_get_status(daddr, port, NULL, process_new_status, STATE_CLEAR_CHANGE);
          break; // after completely processed one port, we will re-queue the status poll and handle next one
        }
      }
    }
  } else if (result == XFER_RESULT_FAILED || result == XFER_RESULT_STALLED || result == XFER_RESULT_TIMEOUT) {
    // A poll that keeps failing never reports a change (e.g. a downstream unplug). Fall back to reading
    // one status per round over EP0, ports 1..bNbrPorts then the hub, so a latched change still goes the
    // normal path. This does not repair the status endpoint itself.
    if (p_hub->bNbrPorts && ++p_hub->poll_failed_count >= HUB_POLL_FAIL_THRESHOLD) {
      p_hub->poll_failed_count = 0;
      const uint8_t port = (p_hub->recover_port < p_hub->bNbrPorts) ? (uint8_t) (p_hub->recover_port + 1) : 0;
      TU_LOG_DRV("HUB status poll keeps failing, addr = %u: read port %u status\r\n", daddr, port);
      processed = hub_port_get_status(daddr, port, get_hub_epbuf(daddr)->ctrl_buf, process_new_status,
                                      port ? STATE_CLEAR_CHANGE : STATE_HUB_STATUS);
      if (processed) {
        p_hub->recover_port = port;
      }
    }
  } else {
    p_hub->poll_failed_count = 0;
  }

  // If new status event is processed: next status pool is queued by usbh.c after handled this request
  // Otherwise re-queue the status poll here
  if (processed) {
    p_hub->busy    = true; // cvcart
    p_hub->busy_ms = tusb_time_millis_api();
  } else {
    TU_ASSERT(hub_edpt_status_xfer(daddr));
  }

  return true;
}

static void process_new_status(tuh_xfer_t* xfer) {
  const uint8_t daddr = xfer->daddr;

  // usbh fails a removed hub's requests after hub_close(): nothing to re-arm
  TU_VERIFY(get_hub_itf(daddr)->ep_in != 0,);

  // a reply shorter than requested (GET_STATUS: 4 bytes, CLEAR_FEATURE: none) fails, so a short status is
  // never decoded from the previous buffer contents
  if (xfer->result != XFER_RESULT_SUCCESS || xfer->actual_len < tu_le16toh(xfer->setup->wLength)) {
    TU_ASSERT(hub_edpt_status_xfer(daddr),);
    return;
  }

  const uint8_t port_num = (uint8_t) tu_le16toh(xfer->setup->wIndex);
  const hub_port_status_response_t *port_status = get_port_status(daddr);
  const uintptr_t state = xfer->user_data;
  bool processed = false; // true if new status is processed

  switch (state) {
    case STATE_HUB_STATUS: {
      hub_status_response_t hub_status = *((const hub_status_response_t *) (uintptr_t) xfer->buffer);
      TU_LOG_DRV("HUB Got hub status, addr = %u, status = %04x\r\n", daddr, hub_status.change.value);
      if (hub_status.change.local_power_source) {
        TU_LOG_DRV("  Local Power Change\r\n");
        processed = hub_clear_feature(daddr, HUB_FEATURE_HUB_LOCAL_POWER_CHANGE,
                                      process_new_status, STATE_COMPLETE);
      } else if (hub_status.change.over_current) {
        TU_LOG_DRV("  Over Current\r\n");
        processed = hub_clear_feature(daddr, HUB_FEATURE_HUB_OVER_CURRENT_CHANGE,
                                      process_new_status, STATE_COMPLETE);
      }
      break;
    }

    case STATE_CLEAR_CHANGE:
      // Get port status complete --> clear change
      if (port_status->change.connection) {
        // Connection change
        // Port is powered and enabled
        //TU_VERIFY(port_status.status_current.port_power && port_status.status_current.port_enable, );

        // Acknowledge Port Connection Change
        processed = hub_port_clear_feature(daddr, port_num, HUB_FEATURE_PORT_CONNECTION_CHANGE,
                                           process_new_status, STATE_CHECK_CONN);
      } else if (port_status->change.port_enable) {
        processed = hub_port_clear_feature(daddr, port_num, HUB_FEATURE_PORT_ENABLE_CHANGE,
                                           process_new_status, STATE_COMPLETE);
      } else if (port_status->change.suspend) {
        processed = hub_port_clear_feature(daddr, port_num, HUB_FEATURE_PORT_SUSPEND_CHANGE,
                                           process_new_status, STATE_COMPLETE);
      } else if (port_status->change.over_current) {
        processed = hub_port_clear_feature(daddr, port_num, HUB_FEATURE_PORT_OVER_CURRENT_CHANGE,
                                           process_new_status, STATE_COMPLETE);
      } else if (port_status->change.reset) {
        processed = hub_port_clear_feature(daddr, port_num, HUB_FEATURE_PORT_RESET_CHANGE,
                                           process_new_status, STATE_COMPLETE);
      }
      break;

    case STATE_CHECK_CONN: {
      const hcd_event_t event = {
        .rhport     = usbh_get_rhport(daddr),
        .event_id   = port_status->status.connection ? HCD_EVENT_DEVICE_ATTACH : HCD_EVENT_DEVICE_REMOVE,
        .connection = {
          .hub_addr = daddr,
          .hub_port = port_num
        }
      };
      hcd_event_handler(&event, false);
      // skip status for attach event, usbh will do it after handled this enumeration
      processed = (event.event_id == HCD_EVENT_DEVICE_ATTACH);
      break;
    }

    case STATE_COMPLETE:
    default:
      processed = false; // complete this status, queue next status
      break;

  }

  if (!processed) {
    TU_ASSERT(hub_edpt_status_xfer(daddr),);
  }
}

//--------------------------------------------------------------------+
// Transaction Translator
//--------------------------------------------------------------------+

// A multi-TT hub runs in its multi-TT alternate setting, with one transaction translator per downstream port. False
// also when that setting is unknown: scheduling it as single-TT is conservative.
//--------------------------------------------------------------------+
// cvcart: reading the ports. A hub reports a change on its status endpoint, but a Genesys GL3523 (the Anker hubs)
// never does: the stack missed every device plugged into it, and every one pulled out. So each port's status is
// also read over EP0, one port every HUB_POLL_MS, and a change goes the way the status endpoint's would.
//--------------------------------------------------------------------+
static void poll_status_cb(tuh_xfer_t* xfer) {
  const uint8_t daddr = xfer->daddr;
  hub_interface_t* p_hub = get_hub_itf(daddr);
  TU_VERIFY(p_hub->ep_in != 0,); // closed

  const hub_port_status_response_t* st = get_port_status(daddr);
  if (xfer->result == XFER_RESULT_SUCCESS && xfer->actual_len >= sizeof(hub_port_status_response_t) &&
      (st->change.value & 0x1F) != 0) { // (bits 0-4 are the change bits)
    tuh_hub_diag_cb(daddr, 2, (uint32_t) tu_le16toh(xfer->setup->wIndex) << 16 | st->change.value);
    tuh_xfer_t as_reported = *xfer;
    as_reported.user_data  = STATE_CLEAR_CHANGE; // as if the status endpoint had named the port
    process_new_status(&as_reported);            // ends with hub_edpt_status_xfer(), here or in usbh
    return;
  }
  p_hub->busy = false;
}

// called by usbh when its events are all handled
void hub_poll(bool enumerating, bool quiet) {
  const uint32_t now = tusb_time_millis_api();
  for (uint8_t i = 0; i < CFG_TUH_HUB; i++) {
    hub_interface_t* p_hub = &hub_itfs[i];
    const uint8_t daddr = (uint8_t) (i + 1 + CFG_TUH_DEVICE_MAX);
    if (!p_hub->ready || p_hub->ep_in == 0 || p_hub->bNbrPorts == 0 || enumerating) {
      continue; // (an enumeration reads its port's status into ctrl_buf too)
    }
    if (p_hub->busy) {
      // a change waits on a transfer or on an enumeration: with neither for a second, it was dropped
      if (quiet && now - p_hub->busy_ms >= 1000) {
        tuh_hub_diag_cb(daddr, 4, 0);
        (void) hub_edpt_status_xfer(daddr);
      }
      continue;
    }
    if ((int32_t) (now - p_hub->poll_at_ms) < 0) {
      continue;
    }
    p_hub->poll_at_ms = now + HUB_POLL_MS;
    const uint8_t port = (uint8_t) (p_hub->poll_port % p_hub->bNbrPorts + 1);
    if (hub_port_get_status(daddr, port, NULL, poll_status_cb, 0)) {
      p_hub->poll_port = port;
      p_hub->busy      = true;
      p_hub->busy_ms   = now;
    }
  }
}

//--------------------------------------------------------------------+
bool hub_tt_multi(uint8_t hub_addr) {
  TU_VERIFY(hub_addr > CFG_TUH_DEVICE_MAX && hub_addr <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB);
  return get_hub_itf(hub_addr)->mtt;
}

//--------------------------------------------------------------------+
// Split ISO Slot Allocation
//--------------------------------------------------------------------+

// Best-effort ISO split windows on one transaction translator: every port of a
// single-TT hub, or one port of a multi-TT hub. Slot s is the microframe Y(s)
// a 188-byte piece is budgeted in, its start-split going out in Y(s-1) as in an
// EHCI H-frame. `used` holds the slots of every open endpoint on the TT, idle
// ones included. OUT grows from slot 0; IN grows from the end, leaving two
// complete retries. Requires 1..564 bytes for IN or 1..1023 for OUT. No
// bus-time admission, interrupt reservations or frame-spanning splits.
bool hub_iso_split_slot(uint8_t used, uint16_t packet_size, uint8_t dir, uint8_t* slot) {
  TU_VERIFY(packet_size >= 1 && packet_size <= (dir == TUSB_DIR_IN ? 564u : 1023u));
  uint8_t const count = (uint8_t) ((packet_size + 187u) / 188u);
  uint8_t const last = (dir == TUSB_DIR_IN ? 4u : 6u) - count;
  for (uint8_t i = 0; i <= last; i++) {
    uint8_t const start = dir == TUSB_DIR_IN ? last - i : i;
    uint8_t const mask = (uint8_t) (((1u << count) - 1u) << start);
    if (!(used & mask)) {
      *slot = start;
      return true;
    }
  }
  return false;
}

#endif
