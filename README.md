# Stadia Controller Dongle

[![Build](https://github.com/preciziei/stadia-dongle-esp-atoms3u/actions/workflows/release.yml/badge.svg)](https://github.com/preciziei/stadia-dongle-esp-atoms3u/actions/workflows/release.yml)
[![Release](https://img.shields.io/github/v/release/preciziei/stadia-dongle-esp-atoms3u?label=firmware)](https://github.com/preciziei/stadia-dongle-esp-atoms3u/releases/latest)
[![License: GPL v3](https://img.shields.io/badge/license-GPL%20v3-blue.svg)](LICENSE)

ESP32-S3 firmware for the **M5Stack AtomS3U** that turns up to three Google Stadia controllers into wired **Xbox 360** gamepads, or into standard HID gamepads for Android. Rumble, Assistant and Capture key actions, a WebHID setup page, and OTA updates are built in. Rumble and the keyboard exist in XInput mode.

> [Flash in the browser](https://preciziei.github.io/stadia-dongle-esp-atoms3u/) · [Configure over USB](https://preciziei.github.io/stadia-dongle-esp-atoms3u/config.html)

Both pages run in desktop Chrome or Edge and talk to the board directly. Nothing is installed on the PC.

## Wi-Fi vs. Bluetooth: Which Dongle Should You Use?

This firmware is designed for Stadia Controllers running **Google's official Bluetooth firmware**, giving you plug-and-play wireless XInput with full rumble and zero controller re-flashing required.

If your Stadia Controller is still on its **original Wi-Fi firmware** (or you want 3.5 mm headset audio and 1000 Hz polling over Wi-Fi), check out **[erikb-lab/stadiacast-esp32](https://github.com/erikb-lab/stadiacast-esp32)**, which covers the Wi-Fi side of the ESP32-S3 dongle ecosystem.


## Quick start

1. Put the AtomS3U in download mode and open the [web installer](https://preciziei.github.io/stadia-dongle-esp-atoms3u/). The board has a single USB-A port.
2. After it reboots, hold **Stadia + Y** on the controller until it flashes orange. A dongle with no saved controllers pairs it on its own.
3. Open the [USB configuration page](https://preciziei.github.io/stadia-dongle-esp-atoms3u/config.html) to remap Assistant and Capture, manage bonds, or start the Wi-Fi page.

The first controller to power on is USB pad 0. The next free pad is used as more controllers connect. Bonds are not pinned to a pad, and the dongle can remember more controllers than it can connect at once (three connected, eight saved).

## What it does

- Up to **3** Stadia controllers at once, each as an Xbox 360 pad (VID `045E`, PID `028E`) with rumble, or as a separate standard HID gamepad (VID `1209`, PID `57A1`) for Android
- Assistant and Capture as a keyboard and media keys, remappable per button and press length
- Setup over USB (WebHID) or over a Wi-Fi page on the dongle
- OTA firmware upload from the Wi-Fi page
- USB remote wake from Assistant, Capture, or A while the PC is asleep
- Status LED in the Stadia orange and white palette

Linux `xpad` treats PID `0289` as an original Xbox pad and misreads these reports, so this firmware uses the Xbox 360 id. Re-plug after flashing so the host drops the old device.

| Stadia | Xbox 360 |
|---|---|
| A / B / X / Y | A / B / X / Y |
| LB / RB | LB / RB |
| LT / RT | LT / RT |
| LS / RS click | LS / RS |
| Sticks and D-pad | Sticks and D-pad |
| Menu | Start |
| Options | Back |
| Stadia | Guide |

Assistant and Capture are not Xbox buttons. Defaults:

| Button | Short press | Long press (1 s) |
|---|---|---|
| Assistant | F14 | Start Web UI |
| Capture | Print Screen | none |

Other actions: F12–F24 (F12 is Steam's screenshot key), Escape, Tab, Space, Enter, Backspace, Insert, Delete, Home, End, Page Up/Down, arrows, volume, mute, and media keys. Hold **Stadia + Options + Menu** for 5 seconds to start the Web UI without remapping.

Each link is negotiated at 11.25–15 ms so three controllers share the radio. Scanning backs off while the Wi-Fi page is up or a controller is already connected. Connections that are already up are not slowed down.

## Hardware

M5Stack AtomS3U (ESP32-S3FN8, 8 MB flash, no PSRAM) or another ESP32-S3 with native USB. ESP-IDF v6.x.

| Function | GPIO | Notes |
|---|---|---|
| Status LED | 35 | WS2812. Change the pin and brightness in menuconfig |
| User button | 41 | Active-low, internal pull-up. Release before 0.5 s starts the Wi-Fi page and requests USB wake when the host is suspended. Hold 0.5 s to switch XInput and HID |
| IR transmitter | 12 | Held low. The LED never fires |
| USB D− / D+ | 19 / 20 | The only USB port. TinyUSB uses it for the gamepads and the keyboard |

The partition table is dual OTA on 8 MB (`partitions.csv`). A 4 MB build is not supported.

## LED

The onboard LED follows the controller's own colors for the states you see every day. Other colors are reserved for things that need attention. Patterns crossfade in about 300 ms.

| Pattern | Meaning |
|---|---|
| Solid orange | Powered, starting up |
| Pulsing orange | Pairing, or no controller saved yet |
| Pulsing white | Connecting to a saved controller |
| Solid white | One controller connected, XInput |
| White with brief dips | Two or three controllers, XInput. One dip per controller |
| Solid orange with the same dips | Controllers connected in HID mode |
| White or orange with a cyan tick | Connected, but nothing on the host has opened the pad |
| Five green strobes | Switching to XInput |
| Five orange strobes | Switching to HID |
| Cyan breathe | Wi-Fi setup page is on and no controller is connected |
| Magenta breathe | The PC has suspended USB |
| Magenta fading toward blue | Firmware update |
| Red pulse | Error |
| Brief white flash | Remote wake was sent |

## Configuration

**USB page.** Plug in the dongle and open [the configuration page](https://preciziei.github.io/stadia-dongle-esp-atoms3u/config.html). Pairing, button actions, the controller list, and live input go over WebHID. Protocol: [docs/usb-config-protocol.md](docs/usb-config-protocol.md).

**Wi-Fi page.** The access point stays off unless you ask for it, because the radio is shared with the controllers. Start it from Assistant's long press, the Stadia + Options + Menu hold, the onboard button, or the USB page. It turns off after a timeout (default 120 seconds) once a controller is connected. Open `http://192.168.4.1` for status, bonds, button actions, and OTA. The page can be set to start on its own when nothing is bonded.

The firmware string on both pages is the UTC build stamp, the same stamp as the GitHub release.

## USB mode

XInput is the default, and it is the mode Windows and Linux should use. Android groups identical XInput interfaces into one pad, so hold the onboard button for 0.5 s to switch to standard HID. The LED strobes (green toward XInput, orange toward HID) and the board restarts. A short press still opens the Wi-Fi page.

HID mode exposes three gamepad interfaces and nothing else, so Android does not invent a ghost device. There is no rumble and no keyboard or WebHID in that mode. The Wi-Fi page still works. The choice is stored in flash. Switching clears the recovery boot counter first, so toggling a few times does not forget paired controllers.

## Windows pad order

XInput mode always exposes three Xbox 360 interfaces, and Windows assigns XInput slots while its driver starts. This build answers the driver's capability request and records the player number Windows sends in the LED command. The Web UI and the USB page show that number as "Windows player N".

That does not by itself force a stable order. After flashing this build once:

1. Open Device Manager, choose View, Show hidden devices.
2. Uninstall the greyed-out **Xbox 360 Peripherals** entries left by older firmware.
3. Unplug the dongle and plug it back in.

Then check a gamepad tester: the controller you power on first should be controller 1, the next controller 2, then 3. If a later re-plug still shuffles them, the remaining cause is the host. `menuconfig` has **Stagger Xbox pad start** for that case. It holds each pad's first report until Windows has assigned the previous pad, or 400 ms have passed. It is off by default. Turn it on only after the checklist's 20-cycle test shows that it helps, because Linux does not send those LED commands and will wait out the timeout.

## Linux

Fedora can runtime-suspend the dongle when no program has a pad open. The LED breathes magenta until something opens a pad or the onboard button requests wake. To keep it awake, add `/etc/udev/rules.d/99-stadia-dongle.rules`:

```
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="028e", TEST=="power/control", ATTR{power/control}="on"
```

Reload udev and re-plug. Magenta means the host really suspended the bus. A white LED with a cyan tick means the host is awake and has not read a report for 3 seconds.

Remote wake needs USB wake enabled in the BIOS and the OS. It is not reliable on every machine. The Stadia button does not send wake, so it can still power the controller off.

## Build

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
```

`sdkconfig.defaults` sets NimBLE central (3 links, 8 bonds), TinyUSB, a 240 MHz CPU, the AtomS3U pins, an 8 MB partition table, and silent logs. The firmware version is the UTC minute of the build. CI passes `STADIA_BUILD_TS`; a local build uses `.stadia-build-ts` if that file exists, otherwise the current UTC time.

To see USB attach, suspend, and report logs, delete `sdkconfig` if a previous configure cached the silent log level, then:

```sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" fullclean build
```

Merged image for the web installer, written at offset 0:

```sh
esptool.py --chip esp32s3 merge_bin \
  -o docs/firmware-merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/stadia-dongle.bin
```

The application offset is `0x20000`, matching `ota_0` in `partitions.csv`. `build/stadia-dongle.bin` is the OTA image.

On Windows the same steps are `build_firmware.cmd`, `flash_firmware.cmd`, `create_merged_fw.cmd`, `flash_merged.cmd`, and `monitor_logs.cmd`. Pass `--no-pause` to skip the prompt at the end. With TinyUSB owning the port after boot, the serial console is only available in download mode unless you wire an external UART.

Host the `docs/` folder and open `index.html` in Chrome or Edge. `docs/serve.cmd` does that locally. Node tests for the WebHID framing live in `tests/` and run with `node --test "tests/**/*.test.mjs"`.

## Releases

A push to `main` builds the firmware, runs the WebHID tests, and publishes a release tagged `build-YYYYMMDD-HHMM` with:

- `stadia-dongle-<stamp>-merged.bin`, the full image used by the web installer
- `stadia-dongle-<stamp>-ota.bin`, the image the Wi-Fi page accepts

The installer page and the USB page are stamped with the same time. Tagged `v*` pushes still get their own release, with the same timestamped filenames. Build tags older than the newest 20 are deleted. Pull requests build and test only.

Manual checks that CI cannot do are in [docs/hardware-test-checklist.md](docs/hardware-test-checklist.md).

## Architecture

```
Stadia BLE ──▶ ble_central.c ──▶ bridge.c ──▶ usb_xbox.c ──▶ Host (Xbox 360 × 3, or HID gamepad × 3)
                   │
                   └──▶ button_actions.c ──▶ hid_extra.c ──▶ Host (keyboard)

Host (rumble, player LED) ──▶ xbox_dev.c ──▶ ble_central.c ──▶ Stadia BLE
```

`xbox_dev.c` is a small TinyUSB class driver. The ESP32-S3 has four IN endpoints besides endpoint 0, and this firmware uses all of them: three gamepads and the utility HID. A fourth gamepad would mean dropping the keyboard, the WebHID page, and remote wake.

## Credits

Built for the AtomS3U, forked from [danzig666/stadia-dongle-esp](https://github.com/danzig666/stadia-dongle-esp), which came from [Scalee/stadia-dongle](https://github.com/Scalee/stadia-dongle). The USB configuration protocol was designed by [jazz-zzzz](https://github.com/jazz-zzzz/stadia-dongle-esp).

## License

[GNU General Public License v3.0](LICENSE)
