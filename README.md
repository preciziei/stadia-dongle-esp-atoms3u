# Stadia Controller Dongle

ESP32-S3 firmware that bridges Google Stadia controllers (Bluetooth LE) to a PC
as a wired **Xbox 360 controller** + **HID mouse** + **HID keyboard**. Wi-Fi
captive portal for pairing, button remapping, and OTA updates.

Custom fit for **M5Stack AtomS3U**, forked from
[danzig666/stadia-dongle-esp](https://github.com/danzig666/stadia-dongle-esp)
(itself from [Scalee/stadia-dongle](https://github.com/Scalee/stadia-dongle)).

> ### ⚡ [Flash the firmware](https://preciziei.github.io/stadia-dongle-esp-atoms3u/) · 🛠️ [Configure over USB](https://preciziei.github.io/stadia-dongle-esp-atoms3u/config.html)
> Both run in desktop Chrome/Edge, talk directly to the device, and need no software installed.

## Requirements

- **M5Stack AtomS3U** (ESP32-S3FN8, **8 MB** flash, no PSRAM) or compatible
  ESP32-S3 with native USB OTG
- WS2812 RGB status LED on **GPIO 35** (configurable in menuconfig)
- Onboard user button on **GPIO 41** (active-low, internal pull-up)
- Onboard IR TX on **GPIO 12** is held low and never driven high
- Single **USB-A** port used for TinyUSB (GPIO 19/20) — no separate UART bridge
- [ESP-IDF v6.x](https://github.com/espressif/esp-idf)

Legacy 4 MB boards can still build with `sdkconfig.defaults.4mb` /
`partitions_4mb.csv`.

## Flashing

The AtomS3U has a **single USB-A** port (USB OTG + Serial/JTAG on the same
PHY). Put the board in download mode, then flash over that port.

No software needed: use the
[web installer](https://preciziei.github.io/stadia-dongle-esp-atoms3u/) in Chrome or
Edge, connect the AtomS3U USB-A port, and click Install. After reboot the same
port enumerates as the Xbox/HID device. The
[USB configuration page](https://preciziei.github.io/stadia-dongle-esp-atoms3u/config.html)
manages pairing and settings over that cable. The installer is the `docs/`
folder, deployed to GitHub Pages from this repo.

## Build

```sh
idf.py set-target esp32s3
idf.py build
```

On Windows, double-click `build_firmware.cmd` for the same build. From a
terminal, pass `--no-pause` to suppress the final prompt.

`sdkconfig.defaults` pre-configures NimBLE central, TinyUSB, 240 MHz CPU,
AtomS3U pins, and production logging (silent). The default partition table fits
**8 MB** flash with dual OTA slots (~3.875 MB each).

To log USB attach, suspend, resume, and Xbox IN completions, build with the
debug overlay (delete `sdkconfig` first if a previous configure cached the
silent log level):

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" fullclean build
```

### Flash

```sh
idf.py -p <PORT> flash
```

On Windows, double-click `flash_firmware.cmd` and enter the COM port. It builds
changed files before flashing. Command-line examples:

```bat
flash_firmware.cmd COM5
flash_firmware.cmd COM5 --no-pause
flash_firmware.cmd COM5 --dry-run --no-pause
```

To view live ESP32 logs, double-click `monitor_logs.cmd` and enter the COM port,
or run `monitor_logs.cmd COM5` from a terminal. Press **Ctrl+]** to stop the
monitor. Run `build_firmware.cmd` first if the build folder is missing. Note:
with TinyUSB owning the USB device after boot, Serial/JTAG console may only be
available in download mode or via an external UART if you wire one.

Merged binary for the [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
installer:

```sh
esptool.py --chip esp32s3 merge_bin \
  -o docs/firmware-merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/stadia-dongle.bin
```

The application offset must be `0x20000` to match `ota_0` in `partitions.csv`.

On Windows, run `create_merged_fw.cmd`. It builds first when required and
creates `docs/firmware-merged.bin` (default **8MB**). Pass `4MB` or `16MB`
only when targeting a different flash size header. `flash_merged.cmd COM5`
writes that image to a device over serial.

Host the `docs/` folder and open `docs/index.html` in Chrome/Edge.

## Setup

1. Flash the firmware over the AtomS3U USB-A port (download mode), then let it
   reboot so TinyUSB enumerates the Xbox/HID device on the same cable.
2. Put the controller in Bluetooth mode (hold **Stadia + Y** until orange
   flash). A dongle with no bonds auto-pairs it -- no page or app needed.
3. Once bonded, the dongle auto-connects on future boots.
4. To remap buttons or change settings, open the
   [USB configuration page](https://preciziei.github.io/stadia-dongle-esp-atoms3u/config.html)
   in Chrome/Edge, or start the Wi-Fi Web UI (below). Short-press the onboard
   button (GPIO 41) to start the Web UI; if the host is sleeping, that press
   also requests USB remote wake.

## Controller Modes

### Gamepad Mode

The controller appears as an **Xbox 360 Controller for Windows** (VID 0x045E,
PID 0x028E). Linux `xpad` maps `0x0289` to the original Xbox report layout, so
this firmware uses the 360 id. Re-plug after flashing so the host drops the
old identity. Button mapping:

| Stadia          | Xbox 360  |
|-----------------|-----------|
| A / B / X / Y   | A / B / X / Y |
| LB / RB         | LB / RB |
| LT / RT         | LT / RT (analog) |
| LS / RS click   | LS / RS |
| Left Stick      | Left Stick |
| Right Stick     | Right Stick |
| D-pad           | D-pad |
| Menu            | Start |
| Options         | Back |
| Stadia button   | Guide |

### Mouse Mode

Hold **Assistant + Capture** for 2 seconds to toggle. Rumble confirms: 2 pulses
= mouse ON, 3 pulses = controller mode restored. Always starts in controller
mode after power-on. While active, the Xbox gamepad interface sends neutral
reports (sticks centered, buttons released) and all input is routed to the HID
mouse.

| Stadia input   | Mouse action                                       |
|----------------|----------------------------------------------------|
| Left stick     | Cursor movement (deadzone, acceleration curve, smoothing) |
| A              | Left click                                         |
| B              | Right click                                        |
| X              | Middle click                                       |
| D-pad up/down  | Scroll wheel (±1 per press)                        |
| Right stick Y  | Analog scroll wheel                                |
| LT (hold)      | Precision mode - cursor speed ÷ 3                  |
| RT (hold)      | Fast mode - cursor speed × 2                       |
| Stadia / LB / RB / LS / RS / Menu / Options | Inactive (neutral Xbox reports) |

Mouse reports are driven by a 125 Hz hardware timer for smooth, jitter-free movement.

### Multi-Controller

Up to **2 controllers** can be paired and used concurrently. Each appears as a
separate Xbox 360 gamepad. Mouse mode and button mappings are per-controller.

BLE connection interval is negotiated for lowest latency:

| Controllers | Connection interval | Update rate |
|-------------|---------------------|-------------|
| 1           | 7.5 ms              | ~133 Hz     |
| 2           | 11 – 15 ms          | ~67 – 91 Hz |

When the Wi-Fi AP is active, BLE scanning pauses to reduce coexistence impact;
existing controller connections and notifications are unaffected.

### Extra Buttons

Assistant and Capture buttons are exposed as a **boot keyboard + consumer-control**
HID interface. Defaults:

| Button    | Short press | Long press (1 s) |
|-----------|-------------|------------------|
| Assistant | F14         | Start Web UI     |
| Capture   | PrintScreen | *(none)*         |

All actions are remappable in the Web UI or over USB. Available actions:
F12–F24 (F12 triggers Steam's screenshot shortcut), PrintScreen, Escape, Space,
Enter, Tab, Backspace, Insert, Delete, Home, End, Page Up/Down, arrow keys,
volume/mute, media playback, remote wake only, and Start Web UI.

## USB Configuration (no Wi-Fi needed)

With the dongle plugged into its USB-A port, open the
[USB configuration page](https://preciziei.github.io/stadia-dongle-esp-atoms3u/config.html)
in Chrome or Edge. It talks to the dongle over WebHID — pairing, button
remapping, the controller list, live input and Wi-Fi control, without joining
the dongle's AP. Protocol details in
[docs/usb-config-protocol.md](docs/usb-config-protocol.md); designed by
[jazz-zzzz](https://github.com/jazz-zzzz/stadia-dongle-esp).

## Web UI

Access at `http://192.168.4.1` when the AP is active. Provides:

- BLE and USB connection status, firmware version
- Per-controller battery, name, address, mouse mode status
- Live button state and raw report bytes
- Pairing controls and bonded controller list
- Button action remapping, long-press duration, Web UI timeout
- Firmware update upload (OTA) - untested
- Reboot and disable-Web-UI

The AP never starts on its own: the 2.4 GHz radio is shared with the
controller links, and USB covers configuration. Start it by holding
**Assistant** on a connected controller, or from the USB configuration page.
It auto-disables after a configurable timeout (default 120 s) when a
controller is connected. The old start-at-boot-until-bonded behaviour can be
re-enabled with the "auto-start when no controller is bonded" option.

## USB Remote Wake

When the host PC sleeps, pressing **Assistant, Capture, or A** sends a remote
wake signal via `tud_remote_wakeup()`. The **Stadia/Guide** button intentionally
does not wake the PC, so it can be held to turn off the controller. Requires
BIOS/OS USB wake support. Not always working.

### Linux autosuspend

Fedora can runtime-suspend the dongle when nothing has the gamepad open. The
LED then breathes magenta, and input stays quiet until a program opens the pad
or the onboard button requests remote wake. That is host policy. To keep the
dongle awake, add `/etc/udev/rules.d/99-stadia-dongle.rules`:

```
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="028e", TEST=="power/control", ATTR{power/control}="on"
```

Reload udev and re-plug. A magenta LED means the host really suspended the
bus. A green LED with a short cyan tick means the host is awake but has not
read an Xbox report for 3 seconds (nothing has the pad open yet).

## LED Reference

Status LED is the onboard WS2812 on **GPIO 35** (AtomS3U). Colors are saturated
and crossfade over about 300 ms. Patterns use full channel levels so blue,
cyan, green, and magenta stay distinct on this LED.

| Pattern                        | Meaning |
|--------------------------------|---------|
| White breathe                  | Booting |
| Blue breathe                   | Scanning, no controller connected |
| Blue fading into cyan          | Scanning + Wi-Fi AP on |
| Fast cyan pulse                | Pairing or connecting |
| Green breathe                  | 1 controller in gamepad mode |
| Green with two bright beats    | 2 controllers in gamepad mode |
| Green with a cyan tick         | Controller connected, host not reading the pad |
| Amber breathe                  | At least 1 controller in mouse mode |
| Magenta breathe                | PC sleeping (USB suspended) |
| Red pulse                      | Error |
| Magenta fading into amber      | OTA firmware update |
| Brief white flash              | Remote wake sent |

## Board button / IR

| Hardware | GPIO | Behavior |
|----------|------|----------|
| User button | 41 | Active-low + pull-up. Short press starts Wi-Fi Web UI; also sends USB remote wake when suspended |
| IR transmitter | 12 | Held as output low — never driven high (IR unused) |
| USB D− / D+ | 19 / 20 | Native USB OTG for TinyUSB (Xbox + HID) |

## Architecture

```
Stadia BLE ──▶ ble_central.c ──▶ bridge.c ──▶ usb_xbox.c ──▶ Host PC (Xbox 360)
                   │
                   ├──▶ button_actions.c ──▶ hid_extra.c ──▶ Host PC (keyboard)
                   │
                   └──▶ mouse_mode.c ──▶ hid_mouse.c ──▶ Host PC (mouse, 125 Hz)

Host PC (rumble) ──▶ xbox_dev.c ──▶ ble_central.c ──▶ Stadia BLE
```

## License

[GNU General Public License v3.0](LICENSE)
