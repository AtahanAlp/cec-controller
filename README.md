# Couch CEC Controller

[![CI](https://github.com/AtahanAlp/cec-controller/actions/workflows/main.yml/badge.svg)](https://github.com/AtahanAlp/cec-controller/actions/workflows/main.yml)

Make a TV behave like a PC monitor: wake it, select the PC input, and put it in
standby automatically. A Raspberry Pi Pico handles HDMI-CEC and a tiny Linux
client runs only at boot, shutdown, suspend, and resume—there is no daemon.

## What you need

- original Raspberry Pi Pico (RP2040);
- spare HDMI cable to cut for CEC;
- USB data cable;
- normal HDMI connection from the PC to the TV;
- Podman or Docker for the reproducible build.

The tested setup is an older Samsung Anynet+ TV, but it uses standard HDMI-CEC
commands and should work with many CEC-enabled TVs.

## 1. Wire the Pico

Only two HDMI wires are used:

| HDMI Type A pin | Signal | Raspberry Pi Pico |
| --- | --- | --- |
| 13 | CEC | GP11, physical pin 15 |
| 17 | DDC/CEC ground | GND, physical pin 18 |

Plug and receptacle views are mirrored. Read the short
[wiring guide](docs/HARDWARE.md) and verify both wires with a continuity meter
before connecting the TV. Do not connect HDMI +5 V or DDC to the Pico.

## 2. Build and flash

```sh
git clone --recurse-submodules https://github.com/AtahanAlp/cec-controller.git
cd cec-controller
./tools/build-firmware
./tools/build-host
```

Hold the Pico's `BOOTSEL` button while plugging in USB, then copy:

```text
build/firmware/pico-cec.uf2
```

to the `RPI-RP2` drive.

## 3. Test before installing

Turn on HDMI-CEC in the TV settings. With exactly one controller attached:

```sh
device=/dev/serial/by-id/usb-Couch_CEC_Couch_CEC_Controller_*-if00

build/host/cecctl --config /nonexistent --device $device protocol
build/host/cecctl --config /nonexistent --device $device detect
build/host/cecctl --config /nonexistent --device $device status
build/host/cecctl --config /nonexistent --device $device standby
build/host/cecctl --config /nonexistent --device $device on
```

If `detect` cannot identify the video input, add its CEC physical address to
the command, for example `--physical-address 3.0.0.0` for TV HDMI 3.

## 4. Enable automatic control on Linux

After both manual power commands work:

```sh
sudo ./tools/install-host
cecctl detect
cecctl status
sudo systemctl start cec-controller-boot.service
```

That enables:

- PC boot/resume → wake TV and select the PC input;
- PC suspend/shutdown → put TV in standby.

The included automatic integration supports Linux with systemd and udev,
including Bazzite, Fedora, Ubuntu, Debian, and Arch-based systems. Other
operating systems can implement the small documented
[USB protocol](docs/PROTOCOL.md).

## Configuration

Host settings live in `/etc/cec-controller.conf`. Most setups need no changes.
Useful fallbacks are:

```ini
# Select one GPU output when several HDMI displays are connected.
connector=card1-HDMI-A-1

# Or set the PC video input directly.
physical_address=3.0.0.0
```

To build for another Pico GPIO or change the TV's device name:

```sh
CEC_GPIO=7 CEC_OSD_NAME="Living Room PC" ./tools/build-firmware
```

Only the original Pico on GP11 has been hardware-tested so far.

## Help and project status

- [Wiring](docs/HARDWARE.md)
- [Linux integration](docs/HOST.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Build details](docs/BUILD.md)

Verified on the test TV: logical-address allocation, power query, standby,
wake, and automatic switch to HDMI 3 using only CEC and ground. The next step
is repeated lifecycle testing on the Bazzite couch PC. Compatibility reports
and native clients for more operating systems are welcome.

This project is derived from [gkoh/pico-cec](https://github.com/gkoh/pico-cec)
and retains its original notices. It is available under the
[MIT License](LICENSE). See [CONTRIBUTING.md](CONTRIBUTING.md) to help.
