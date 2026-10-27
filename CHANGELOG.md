# Changelog

## Gamepad support and AtomS3U Gamepad flasher profile

- **Added** a composite USB HID gamepad alongside the existing keyboard and mouse,
  using the ESP32-S3 Arduino core's `USBHIDGamepad` device.
- **Added** BLE and WiFi gamepad reports with button masks, D-pad hat, and six axes,
  plus browser controls in both control pages.
- **Added** a 600 ms input watchdog and release-on-disconnect behavior so a stale
  controller connection cannot leave buttons or axes held indefinitely.
- **Added** a fourth installer choice on the hosted page: a separately built
  AtomS3U Gamepad profile with its own manifest and firmware artifact directory.
- **Status:** firmware and UI are implemented; gamepad enumeration and Steam Deck
  behavior still require hardware verification.

## All three boards hardware-verified (current)

- **Fixed** LilyGo T-Dongle S3 screen (was completely black on real hardware): the
  backlight pin is active-low (`digitalWrite(TFT_BL, LOW)` turns it on — the code had
  it inverted), and the ST7735 init needs `INITR_MINI160x80_PLUGIN` instead of plain
  `INITR_MINI160x80` (the plain variant forces the wrong RGB color order, a 2px offset,
  and no baked-in inversion). Confirmed against LilyGo's own factory TFT_eSPI example
  and official `Setup209_LilyGo_T_Dongle_S3.h` config.
- **Changed** board-specific code (pins, display init/render) moved out of the shared
  `.ino` into `firmware/keyboard-dongle/boards/<board>.h`, one header per board, so a
  change to one board's screen can't affect another's.
- **Changed** CI (`build-firmware.yml`) now rebuilds only the board(s) whose files
  changed (via `dorny/paths-filter`) instead of all three on every push.
- **Status:** all three boards (LilyGo T-Dongle S3, Waveshare ESP32-S3-Geek, M5Stack
  AtomS3U) are hardware-verified — USB keyboard, USB mouse, WiFi AP, and BLE control
  all confirmed working. Project is functionally complete.

## Reconstruction — multi-board ESP32-S3

The project was reorganized around **three ESP32-S3 boards**, sharing one firmware.

- **Added** `firmware/keyboard-dongle/` — a single sketch that targets all three boards
  via `#define BOARD` (or `-DBOARD=n`). USB-HID + WiFi + BLE are identical across
  boards; only the screen driver differs:
  - `1` LilyGo T-Dongle S3 — ST7735 80×160
  - `2` Waveshare ESP32-S3-Geek — ST7789 240×135 (pins verified from Waveshare/Tasmota docs)
  - `3` M5Stack AtomS3U — no screen (fixed WiFi password `type12345`)
- **Added** a board-picker web flasher (`web/index.html` + `manifest-<board>.json` +
  `web/<board>/` bins) hosted at https://kolec94.github.io/esp32-s3-hid-dongle/web/
- **Changed** `REQUIRE_BLE_PAIRING` default to `0` (open BLE control) for reliable first
  boot; set to `1` to require an on-screen pairing PIN.
- **Removed** the ESP32-C5 sketches and the superseded single-mode S3 sketches.
- **Added** USB **mouse** (HID) alongside the keyboard — trackpad + click/scroll in
  `control.html`; `0x02` command over BLE and a `/mouse` WiFi endpoint.
- **Added** a **button** (BOOT/GPIO0) that toggles WiFi↔BLE live; screen shows the mode.
- **Status:** **Waveshare ESP32-S3-Geek is hardware-verified** (keyboard, mouse,
  screen, button, WiFi + BLE all working). LilyGo T-Dongle S3 and M5Stack AtomS3U are
  compile-only so far — same firmware, screen/button wiring unverified on those panels.
  See the verification table in the README.

## Background — why it changed

- Project began as a USB-HID keyboard for the **LilyGo T-Dongle S3**.
- Hardware ordered turned out to be an **ESP32-C5**, which has **no USB-OTG** and so
  physically cannot be a USB keyboard (only ESP32-S2/S3 can).
- A wireless proof-of-concept was built on the C5 (WiFi web page → BLE HID keyboard).
  It worked on Linux but was unreliable: BLE dropped under WiFi+BLE coexistence and
  couldn't auto-reconnect because BLE bonding crashes the C5's BLE stack in
  arduino-esp32 3.3.10 (and NimBLE-Arduino won't link against the C5 core).
- Decision: keep the C5 as a dual-band WiFi analyzer; get **ESP32-S3** hardware for the
  real wired-USB keyboard. Three S3 boards were ordered, hence this multi-board layout.
