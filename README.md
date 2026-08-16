# Couch CEC Controller

An RP2040-based HDMI-CEC bridge that makes a Samsung TV behave like the
monitor of a Bazzite couch PC. The PC sends small commands over USB; a
Raspberry Pi Pico transmits the corresponding CEC messages to the TV.

The primary behavior is deliberately narrow:

- wake the TV and select the PC's HDMI input when the PC boots or resumes;
- put the TV in standby before the PC suspends or shuts down;
- require no continuously running host daemon.

Volume and other remote-replacement controls can be added after the power and
input-selection path is reliable.

## Initial topology

The first prototype uses two independent TV HDMI inputs:

```text
PC GPU ---------------- normal HDMI cable ----------------> TV video input
   |
   +-- USB --> Raspberry Pi Pico --> CEC-only HDMI cable --> TV spare input
```

Only these conductors are required in the CEC-only cable:

| HDMI pin | Signal | Pico connection |
| --- | --- | --- |
| 13 | CEC | Configured 3.3 V GPIO |
| 17 | DDC/CEC ground | GND |

DDC pins 15 and 16 are intentionally disconnected in the first prototype.
The host can obtain the physical address of the real video connection from
the GPU connector's EDID and pass it to the Pico over USB. Reading EDID on the
spare CEC-only input would identify the wrong HDMI port.

> [!CAUTION]
> RP2040 GPIO is not 5 V tolerant. Do not connect HDMI DDC pins 15/16 or HDMI
> +5 V pin 18 directly to the Pico. Any later DDC experiment must use suitable
> bidirectional level shifting. Pin 18, if a particular TV needs it for HDMI
> presence detection, must use a separately reviewed protected interface.

## Baseline

The firmware starts from [gkoh/pico-cec](https://github.com/gkoh/pico-cec),
which provides an RP2040-native, interrupt-driven CEC implementation, USB CDC
command interface, logical-address allocation, and persistent configuration.
The first target is the original Raspberry Pi Pico, with a manually configured
physical address and no DDC connection.

Samsung/Anynet+ behavior will be validated using this sequence:

1. claim a Playback Device logical address;
2. advertise the PC video port with `Report Physical Address`;
3. send `Image View On` to the TV;
4. wait for the TV to wake;
5. broadcast `Active Source` for the PC video port.

Power-off uses the directed `Standby` command. Exact delays and retries will
be tuned on the target TV rather than assumed from its unknown model number.

See [docs/PLAN.md](docs/PLAN.md) for the staged implementation and acceptance
gates, [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the protocol and host
integration design, [docs/BUILD.md](docs/BUILD.md) for the reproducible
firmware build and baseline smoke test, and [docs/PROTOCOL.md](docs/PROTOCOL.md)
for the versioned USB commands.

## Project state

The purpose-built original-Pico firmware and `CECCTRL/1` USB protocol are
buildable, and the native command-sequence tests pass. TV behavior is not yet
validated. Nothing should be connected to the TV until the wiring is
continuity-checked and the firmware image has been flashed and inspected over
USB.

## Upstream and license

This repository is derived from `gkoh/pico-cec` and retains its Git history.
Its MIT license and original copyright notice are in [LICENSE](LICENSE).
The `upstream` Git remote tracks the original project.
