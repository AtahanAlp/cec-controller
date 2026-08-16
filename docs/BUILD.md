# Firmware build and baseline smoke test

## Prerequisites

The build uses Podman, which is already part of the intended Bazzite workflow.
The compiler and SDK tooling stay inside a Fedora container; no packages need
to be layered onto the host OS.

Clone the pinned dependencies once after cloning the repository:

```sh
git submodule update --init --recursive FreeRTOS-Kernel pico-sdk tcli
```

The recursive command currently fetches some dependency test and wireless
submodules that this firmware does not compile. Their revisions are still
pinned by the parent projects, and nothing is installed outside the checkout
or Podman's normal image storage.

## Build

Run:

```sh
./tools/build-firmware
```

The script builds a reusable local image named
`localhost/cec-controller-builder:fedora42`, then writes the firmware to:

```text
build/firmware/pico-cec.uf2
```

The configured baseline is:

| Setting | Value |
| --- | --- |
| Board | original Raspberry Pi Pico / RP2040 |
| CEC GPIO | GP11, physical Pico pin 15 |
| USB | CDC shell plus upstream HID interface |
| Device type | Playback Device |
| Logical address | automatic, preferring 4 then 8 then 11 |
| Physical address | manual, initially unset (`0.0.0.0`) |
| DDC | compiled out |
| Status LED | Pico's onboard LED; WS2812 output compiled out |

The CEC HDMI pin 13 wire goes to Pico GP11. HDMI pin 17 goes to any Pico GND,
for example physical pin 18 next to GP11. Do not connect the hardware while
continuity testing the cut cable.

To remove the reusable builder image later:

```sh
podman image rm localhost/cec-controller-builder:fedora42
```

## Flash

1. Disconnect the Pico from the TV-side cable.
2. Hold `BOOTSEL` while connecting the Pico to the PC over USB.
3. Copy `build/firmware/pico-cec.uf2` onto the `RPI-RP2` USB drive.
4. Wait for the drive to disappear and the Pico to enumerate again.

## Configure the physical address

Connect a serial terminal at 115200 baud to the stable device symlink under
`/dev/serial/by-id/`. Use the physical address of the TV input carrying the
PC's real video signal. For HDMI input 1, the value is usually `1000`; confirm
it rather than relying on the example.

Enter these commands one line at a time:

```text
show version
show config
set config physical_address 1000
set config edid_delay_ms 0
save
reboot
```

Reconnect the terminal after reboot and confirm that the running configuration
is no longer `0.0.0.0`:

```text
show cec
show nvs
```

## First CEC smoke test

Only after the cut cable has passed continuity and short-circuit checks,
connect it to the TV's spare HDMI input. With an allocated Playback Device
logical address, these raw commands test the planned Samsung sequence for a
PC video physical address of `1.0.0.0`:

```text
send f 84 10 00 04
send 0 04
send f 82 10 00
show stats cec
```

Allow roughly 1.5 seconds between `send 0 04` and `send f 82 10 00` during the
manual test. To request standby:

```text
send 0 36
show stats cec
```

An incrementing `CEC tx noack` count is useful evidence, not a reason to add
more HDMI wires blindly. Record it alongside the TV behavior before moving to
a fallback.
