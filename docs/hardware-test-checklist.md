# Hardware Test Checklist

Manual pre-release checks. Most changes in this firmware can only be verified
with a real controller and host, so run the sections your change touches — and
Boot + Gamepad always — before tagging a release.

Idea borrowed from jazz-zzzz/stadia-dongle-esp.

## Boot & USB

- [ ] Fresh flash via the web installer boots (LED: white breathe, then blue breathe).
- [ ] Dongle enumerates as **Xbox 360** `045e:028e` on the AtomS3U USB-A port (TinyUSB).
- [ ] On Fedora, `dmesg` shows `xpad` bind the device as an Xbox 360 pad and does not repeat USB disconnects. `evtest` / a browser gamepad tester shows input, and the LED stays green (no magenta flicker).
- [ ] Onboard RGB LED (GPIO 35) shows the README colors: blue scan, cyan pair, green play, magenta only while the host is actually suspended.
- [ ] Onboard button (GPIO 41) starts the Wi-Fi Web UI; when the host is suspended it also requests remote wake.
- [ ] IR pin (GPIO 12) stays inactive (no IR activity).
- [ ] `joy.cpl` (or a gamepad tester) shows the pad with no phantom input at rest.
- [ ] After a reboot with bonds stored, the Wi-Fi AP does **not** start.
- [ ] On a fresh flash (no bonds), the setup AP **does** start.

## Pairing

- [ ] Pairing from the Web UI or USB config page connects a controller in
      Bluetooth mode (Stadia + Y, orange flash).
- [ ] Controller auto-reconnects after power-off/on in under ~5 s.
- [ ] Second controller pairs and appears as a second gamepad.
- [ ] Forget + re-pair works without a dongle reboot.

## Gamepad

- [ ] All buttons, sticks, triggers and D-pad register correctly.
- [ ] Sticks reach full deflection in all directions; center is neutral.
- [ ] Rumble works (test in a game or the Xbox 360 controller test dialog).
- [ ] After a controller disconnects mid-session, it reconnects as the **same**
      gamepad index, and inputs are neutral while it is away.

## Mouse mode

- [ ] Assistant + Capture (2 s hold) toggles mouse mode; 2 rumble pulses on,
      3 pulses off.
- [ ] Cursor speed is the same diagonally as straight (draw slow circles).
- [ ] Slow stick deflection moves the cursor; releasing stops it dead.
- [ ] Right-stick scroll ramps smoothly from slow to fast; D-pad up/down
      scrolls one detent per press.
- [ ] A / B / X click as left / right / middle.

## Extra buttons

- [ ] Assistant and Capture short/long presses emit their configured keys.
- [ ] Remapping via the Web UI and via the USB config page both persist across
      a reboot.

## Web UI / USB config

- [ ] Captive portal pops automatically after joining the AP.
- [ ] USB config page (docs/config.html) connects, shows live status, saves
      config, lists controllers, and live input reflects button presses.
- [ ] AP auto-off fires after the configured timeout with a controller
      connected.

## Power

- [ ] Host sleep: dongle LED breathes magenta; Assistant/Capture/A wakes the PC
      (BIOS/OS USB wake enabled); Stadia button does **not** wake it.
- [ ] With a controller connected and no program reading the pad, the LED stays
      green and ticks cyan. It does not turn magenta unless the host suspends USB.
- [ ] After host resume, controllers work without re-pairing and without a USB
      re-enumeration storm (`dmesg` stays quiet).
