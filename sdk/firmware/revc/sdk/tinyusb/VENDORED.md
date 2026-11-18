# TinyUSB, for the plugins' USB game pads

From https://github.com/hathach/tinyusb, `master` at commit
`ad7c83db8a11f7f05869eb39c10b0f158c77880b` (2026-11-15: release 0.21.0 of
2026-06-30 and the fixes since), MIT (`LICENSE`). A plugin that includes
`../cvpad_tinyusb.cmake` before `pico_sdk_import.cmake` builds its USB host
from here instead of the pico-sdk's TinyUSB 0.18 (`PICO_TINYUSB_PATH`).

## Why not the pico-sdk's 0.18

With 0.18 as the host, the NES plugin on the console (0.4.2) stopped for
half a second at a time and then went back to the menu with `panic Can't
continue xfer on inactive ep`, when a pad was already plugged in at the
start.

- 0.18's enumeration waits inside `tuh_task()`: 500 ms for a device on the
  port, 450 ms for each one behind a hub, 100 ms before a retry
  (`tusb_time_delay_ms_api`, a `sleep_ms`). Since 0.21 they are deferred
  (`usbh_defer_func_ms_async`), and `tuh_task()` returns at once: on the
  console the pads' task's longest run was 0.05 ms (NES 0.4.3).
- 0.18's RP2040 host driver panics on a buffer that completes for an
  endpoint with no transfer (after a STALL, a timeout or a device going
  away), never ends a control transfer that timed out, and leaves
  enumeration stuck for good when the device at address 0 is unplugged.

## Why not 0.21.0 as released

NES 0.4.3 had 0.21.0: no stops and no panic on the console, but a Switch
Pro Controller was found on some runs and not on others. 0.21.0's RP2040
host driver was new in it (rewritten March to May 2026), and what has been
fixed in it since is what a session of plugging pads in and out meets:

- `hcd_device_close()` left a closed endpoint's interrupt slot number in
  its pool entry. The entry's next owner (a control endpoint, say) was then
  taken for an interrupt endpoint, and a completed buffer could be given to
  the stale entry instead of the live one: the pad's reports, or the
  Switch start's replies, never arrive. Which devices went in and out
  before decides whether it happens. (3174e760, #4177.)
- A STALLed or timed-out control transfer left its buffer AVAILABLE and the
  next transfer panicked; `hcd_edpt_abort_xfer()` was a stub, so usbh's
  control watchdog couldn't end a transfer (e2048238, #4166).
- The HOST_RESUME interrupt was enabled and never acknowledged (38bd85a0).

## What is here

`src/` without the other families' `portable/` drivers, and the files of
`hw/bsp` that the pico-sdk's build reads.

Changed, each marked `cvcart`:

- `src/host/hub.c`: Get Hub Descriptor asks for descriptor type 0x29 in
  wValue (USB 2.0 11.24.2.5), not 0, which a VIA VL817 hub (2109:2817)
  stalls. (`../../midihello/tinyusb_fix/hub.c` is the same fix for 0.18.)
- `src/portable/raspberrypi/rp2040/hcd_rp2040.c`: a DATA_SEQ error (a
  packet with the wrong DATA0/1) fails the EPX transfer as an RX timeout
  does, instead of `panic("Data Seq Error")`. Pulling a pad out of the hub
  during Mega Man 2 (NES 0.4.5) did that: the menu's note read
  "panic Data Seq Error 200244ab", hcd_rp2040.c's panic line.
