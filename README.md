# ESP32-S3 USB-HID Keyboard Dongle

Turn an **ESP32-S3** board into a remote **USB HID keyboard, mouse, and gamepad**.
Plug it into a target computer, then send text, pointer, or controller input from
another machine over **BLE** or **WiFi**.

```
[Control machine] --BLE / WiFi--> [ESP32-S3 dongle] --USB HID keyboard/mouse/gamepad--> [Target PC]
```

> Needs an **ESP32-S3** (or S2). The USB-keyboard trick requires native **USB-OTG**,
> which the S-series has and the C-series (C3/C5/C6) does **not**.

## Supported boards

One firmware (`firmware/keyboard-dongle`) targets three boards. The hosted flasher
offers a fourth profile: an AtomS3U build with gamepad support. Select a target with
`#define BOARD` (or `-DBOARD=n`).

| Board | `BOARD` | Screen | Notes |
|-------|---------|--------|-------|
| **LilyGo T-Dongle S3**     | `1` | ST7735 80×160  | USB-A plug + screen |
| **Waveshare ESP32-S3-Geek**| `2` | ST7789 240×135 | USB-A plug + screen (default) |
| **M5Stack AtomS3U**        | `3` | none           | USB-A plug; fixed WiFi creds, no on-screen display |
| **M5Stack AtomS3U Gamepad** | `4` | none          | Same AtomS3U hardware; gamepad firmware profile |

### Verification status

All three boards have been hardware-verified for USB keyboard/mouse, screen (where
present), button WiFi↔BLE toggle, and WiFi + BLE control:

| Board | Status |
|-------|--------|
| **Waveshare ESP32-S3-Geek** | ✅ **Hardware-verified** |
| **LilyGo T-Dongle S3** | ✅ **Hardware-verified** |
| **M5Stack AtomS3U** | ✅ **Hardware-verified** (no screen; fixed pass `type12345`) |

Gamepad firmware and control UI are implemented on this branch; gamepad enumeration
and Steam Deck behavior still need hardware verification.

## Quick start (one-click flasher, no toolchain)

1. In **Chrome/Edge**, open **https://kolec94.github.io/esp32-s3-hid-dongle/web/**
2. Pick your board, hold the board's **BOOT** button, plug it in, click **Install**.
3. Plug it into the **target** computer. With a screen, it shows the WiFi
   name/password; screenless boards use a fixed password (below).
4. Connect from your phone/laptop over WiFi or BLE to **type and move the mouse**.
   For gamepad input, select the **AtomS3U Gamepad** profile and open the control page.
   Press the board's button to switch **WiFi ⇄ BLE** (screen shows the active mode).

**Screenless (AtomS3U) default credentials:** WiFi SSID `Keeb-XXXX` (XXXX = chip id,
visible in the WiFi list), password **`type12345`**, page at `http://192.168.4.1`.

## Build from source

Arduino IDE or arduino-cli, **esp32 core 3.x**. Board **ESP32S3 Dev Module** with:
USB CDC On Boot **Enabled**, USB Mode **USB-OTG (TinyUSB)**, Partition Scheme
*16M Flash (3MB APP/9.9MB FATFS)*, Flash Size **16MB**. Libraries: **Adafruit GFX**,
**Adafruit ST7735 and ST7789**.

Set your board at the top of `firmware/keyboard-dongle/keyboard-dongle.ino`
(`#define BOARD ...`), then upload. Or headless per board:

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB,FlashSize=16M" \
  --build-property "compiler.cpp.extra_flags=-DBOARD=2" \
  firmware/keyboard-dongle
# BOARD=1 LilyGo, 2 Geek, 3 AtomS3U, 4 AtomS3U Gamepad
```

> **AtomS3U has only 8 MB flash** (vs 16 MB on the others), so build it with
> `FlashSize=8M` + `PartitionScheme=default_8MB`. `collect-bins.ps1` does this
> automatically per board.

**CI:** a GitHub Action (`.github/workflows/build-firmware.yml`) rebuilds **only the
board/profile(s) that changed** and commits the refreshed `web/<profile>/` bins:
- edit `boards/geek.h` → builds **just the Geek**
- edit the shared `keyboard-dongle.ino` → builds **all** (it affects every board)

So a push to `main` keeps the hosted flasher in sync — `collect-bins.ps1` stays for
local/offline builds (it builds all four firmware profiles).

## Files

| Path | What |
|------|------|
| `firmware/keyboard-dongle/keyboard-dongle.ino` | Shared logic (keyboard, mouse, gamepad, BLE, WiFi, web). |
| `firmware/keyboard-dongle/boards/<board>.h` | Each board's **isolated** hardware: pins, screen driver, button. Edit one without touching the others. |
| `web/index.html` | Hosted page: flash any board + drive it over BLE. |
| `web/control.html` | Standalone BLE keyboard, mouse, and gamepad control page. |
| `web/<profile>/` + `web/manifest-*.json` | Pre-built bins per flasher option. |
| `collect-bins.ps1` | Local build of all four firmware profiles into `web/`. |
| `.github/workflows/build-firmware.yml` | CI: rebuild + commit bins on push. |
| `CHANGELOG.md` | History of the project and changes. |

## Notes / caveats

- **Controls:** the device is a composite USB **keyboard + mouse + gamepad**. The hosted
  **`control.html`** page controls text, special keys, a mouse trackpad, and a gamepad
  (D-pad, buttons, and stick axes) over BLE. The WiFi page at `http://192.168.4.1`
  exposes the same controls. Gamepad button/axis state is released after 600 ms without
  an update, and also when the active radio connection drops.
- **Gamepad protocol:** send a 13-byte report to HTTP `POST /gamepad` or the BLE RX
  characteristic: `0x03, 'g', x+128, y+128, z+128, rz+128, rx+128, ry+128, hat, buttons[0..3]`.
  The final four bytes are a little-endian 32-bit button mask; hat values are 0 for
  centered and 1–8 clockwise from up.
- **Button switches the radio:** the BOOT button toggles **WiFi ⇄ BLE**; only one is live
  at a time and the screen shows which. (It does *not* auto-switch on connect.)
- **BLE is a data service, not an OS keyboard.** Connect via `control.html` (Web
  Bluetooth) — it will **not** show in your OS Bluetooth "add device" list, by design.
  Web Bluetooth works in **Chrome/Edge** (Windows/macOS/Linux/Android); not iOS or Safari/Firefox.
- **BLE pairing/security is OFF by default** (`REQUIRE_BLE_PAIRING 0`) for reliable first
  boot. Set `1` (needs a screen) for an on-screen pairing PIN — less tested.
- **US keyboard layout** is assumed on the target for symbols.
- **Screens:** all hardware-verified. Per-board display code lives in
  `firmware/keyboard-dongle/boards/<board>.h`. Geek's ST7789 needs the
  **software-SPI constructor** (hardware-SPI re-init lands on the wrong pins → blank).
  LilyGo's ST7735 needs `INITR_MINI160x80_PLUGIN` (not plain `INITR_MINI160x80`) for
  the correct BGR color order/offset/inversion, and its backlight pin is
  **active-low** (`LOW` = on) — both confirmed against LilyGo's own factory example.
- **Flashing an S3:** while the firmware is running it won't auto-reset into the
  bootloader over native USB — **hold BOOT while plugging in** to force download mode,
  then flash. (The running app also re-enumerates to a different COM port than the bootloader.)
