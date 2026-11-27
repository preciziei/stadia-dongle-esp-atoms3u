# Hardware Test Checklist

Manual checks. The firmware cannot be proven from CI alone. Run Boot, Pairing,
and Gamepad before trusting a build, and the sections your change touches.

## Boot and USB

- [ ] Web installer flash boots. LED is solid orange, then pulsing orange when nothing is bonded.
- [ ] Dongle enumerates as Xbox 360 `045e:028e` on the AtomS3U USB-A port.
- [ ] Windows Device Manager shows three Xbox 360 pads and one HID keyboard. After the first flash of this 3-pad build, remove greyed-out "Xbox 360 Peripherals" entries (View, Show hidden devices) and re-plug once.
- [ ] On Fedora, `dmesg` shows `xpad` bind three Xbox 360 interfaces and does not repeat USB disconnects.
- [ ] `joy.cpl` or a gamepad tester shows no phantom input at rest.
- [ ] Onboard button (GPIO 41): release before 0.5 s starts the Wi-Fi Web UI. When the host is suspended it also requests remote wake.
- [ ] IR pin (GPIO 12) stays inactive.
- [ ] Web UI and USB config page show the firmware version as the UTC build stamp (`YYYYMMDD-HHMM`).

## Pairing

- [ ] A dongle with no bonds pairs a controller that is in Bluetooth mode (Stadia + Y, orange flash) without opening a page. LED pulses orange, then goes solid white.
- [ ] A bonded controller reconnects after power-off in under about 5 seconds. LED pulses white while it connects, then solid white.
- [ ] Second and third controllers connect. The first one powered on takes USB pad 0.
- [ ] Forget and re-pair works without rebooting the dongle.
- [ ] More than three controllers can stay bonded. Only three are connected at once.

## Gamepad

- [ ] Buttons, sticks, triggers, and D-pad register on each connected pad.
- [ ] Sticks reach full deflection. Center is neutral.
- [ ] Rumble works on each pad.
- [ ] After a controller disconnects, its pad goes neutral, and the next controller to power on takes the lowest free pad.

## Windows player order

Do this in Edge at a gamepad tester, with the staggered-start option still off.

- [ ] Power on controller A, then B, then C. The tester shows them as controller 1, then 2, then 3.
- [ ] The Web UI and USB config page show "Windows player 1/2/3" matching the tester.
- [ ] Unplug and re-plug the dongle 20 times without changing which controller is on. Record whether the player numbers stay in power-on order.
- [ ] If they do not, enable `Dongle USB staggered pad start` in menuconfig, rebuild, and repeat the 20 cycles. Keep the option only if that run is stable. Linux will not send LED commands, so the option delays pads 2 and 3 by up to 400 ms.

## Extra buttons

- [ ] Assistant and Capture short and long presses emit their configured keys.
- [ ] Remapping from the Web UI and from the USB config page both survive a reboot.
- [ ] Holding Stadia + Options + Menu for 5 seconds starts the Web UI.

## Web UI and USB config

- [ ] The USB config page connects, shows live status, saves config, lists controllers, and live input follows button presses.
- [ ] The captive portal opens after joining the AP.
- [ ] The AP turns itself off after the configured timeout while a controller is connected.

## LED

- [ ] Solid orange while the dongle is booting.
- [ ] Pulsing orange while pairing, and while no controller is bonded.
- [ ] Pulsing white while connecting to a bonded controller.
- [ ] Solid white with one controller in XInput. Two brief dips with two controllers, three with three.
- [ ] HID mode uses the same dip count in orange. The cyan host-idle tick still appears.
- [ ] A cyan tick on the white pattern when a controller is connected and nothing has the pad open.
- [ ] Cyan breathe when the Wi-Fi page is on and no controller is connected.
- [ ] Magenta breathe only while the host has suspended USB.
- [ ] Magenta fading toward blue during an OTA update.
- [ ] A short white flash when remote wake is sent.

## USB mode switch (G41)

- [ ] Hold G41 for 0.5 s. The LED strobes immediately (5 green = switching to XInput, 5 orange = switching to HID) and the release does nothing.
- [ ] After the restart the dongle enumerates in the other mode. XInput stays `045e:028e` with three Xbox pads plus the keyboard. HID is `1209:57a1` with three gamepads named Gamepad 1, 2, and 3, and no keyboard.
- [ ] On Android 15, hardwaretester.com and RetroArch show one distinct pad per connected controller. No ghost "Microsoft Controller" pad. Two controllers do not share one pad's input.
- [ ] Rumble and Assistant/Capture key actions work after switching back to XInput. They do nothing in HID mode. The Wi-Fi page works in both.
- [ ] Pairings survive several toggles. The recovery boot counter must not forget bonds.

## Power

- [ ] Host sleep: LED breathes magenta. Assistant, Capture, or A wakes the PC when USB wake is enabled in the BIOS and OS. The Stadia button does not wake it.
- [ ] After the host resumes, controllers work without re-pairing and without a stream of USB disconnects in `dmesg`.
