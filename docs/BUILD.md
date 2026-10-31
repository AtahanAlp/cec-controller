# Building the firmware and host client

## Clone dependencies

The Pico SDK, FreeRTOS kernel, and command parser are pinned as Git submodules:

```sh
git clone --recurse-submodules https://github.com/AtahanAlp/cec-controller.git
cd cec-controller
```

For an existing clone:

```sh
git submodule update --init --recursive
```

## Reproducible container build

The supplied tools automatically prefer Podman and fall back to Docker. The
compiler and Pico SDK tooling stay inside a Fedora builder image; nothing is
installed on the host.

Build and test everything:

```sh
./tools/test-firmware
./tools/build-firmware
./tools/build-host
```

Outputs are written only under `build/`:

```text
build/firmware/pico-cec.uf2
build/host/cecctl
```

Set `CEC_CONTAINER_ENGINE=docker` or `CEC_CONTAINER_ENGINE=podman` to choose
explicitly.

## Firmware settings

The supported default is:

| Setting | Default |
| --- | --- |
| Board | original Raspberry Pi Pico / RP2040 |
| CEC GPIO | GP11, Pico physical pin 15 |
| OSD name | `Couch PC` |
| Device type | Playback Device |
| Logical address | automatic, preferring 4, 8, then 11 |
| Physical address | supplied at runtime by the host |
| DDC | disabled |
| Status LED | Pico onboard LED |

Override safe build settings through the environment:

```sh
CEC_GPIO=7 \
CEC_OSD_NAME="Living Room PC" \
CEC_BOARD=pico \
./tools/build-firmware
```

The script rejects invalid GPIO numbers and OSD names longer than 14
characters. Other Pico SDK board definitions may compile, but only the
original Pico is currently hardware-validated.

## Flash the Pico

1. Disconnect the CEC-only HDMI cable from the TV.
2. Hold `BOOTSEL` while connecting the Pico to the PC over USB.
3. Copy `build/firmware/pico-cec.uf2` to the `RPI-RP2` drive.
4. Wait for the drive to disappear and the USB serial device to enumerate.

The runtime USB identity is `cafe:4001`. Linux should create a stable path
similar to:

```text
/dev/serial/by-id/usb-Couch_CEC_Couch_CEC_Controller_<serial>-if00
```

## Firmware smoke test

Open the stable serial path at 115200 baud and run:

```text
show version
show cec
tv protocol
```

Expected protocol reply:

```text
CECCTRL/2 OK command=protocol firmware=<git-version>
```

After completing the continuity checks in [HARDWARE.md](HARDWARE.md), connect
the TV and test `tv status` before any power-changing command.

## Native host build

The Linux client requires a C11 compiler, CMake 3.20 or newer, and a build
tool. A native build does not require the ARM toolchain or Pico SDK:

```sh
cmake -S host -B build/host-native -DCMAKE_BUILD_TYPE=Release
cmake --build build/host-native
ctest --test-dir build/host-native --output-on-failure
```

The resulting `build/host-native/cecctl` links only against the system C
library. The container-built client uses glibc.

## Cleaning build output

All generated project files are contained under `build/`, which is ignored by
Git. Remove that directory when a completely clean rebuild is desired. The
reusable container image is named:

```text
localhost/cec-controller-builder:fedora42
```

Remove it with the same engine used to build, for example:

```sh
podman image rm localhost/cec-controller-builder:fedora42
```

## Identifying the GPIO build

The default build matches the original wiring guide: CEC on GP11 (Pico
physical pin 15) and ground on physical pin 18. The enclosed couch-PC
controller uses GP1 (physical pin 2) and ground on physical pin 3; build for it
with `CEC_GPIO=1 ./tools/build-firmware`. Set `CEC_GPIO` to match any other
custom wiring.

The builder also writes a named artifact such as
`couch-cec-pico-gp1-protocol2-<version>.uf2` and a `.manifest` containing the
board, GPIO, protocol, version, OSD name, and firmware checksum. Update the host
client together with firmware when changing protocol versions.

The SDK's picotool build is pinned to the official Raspberry Pi picotool 2.2.0
source commit `a7eb3988f0645239185fadb4e25d8279478c2dbb`. It is built inside
the existing project builder; no host picotool installation is required.

For remote maintenance, `python3 tools/pico-maintenance.py console` reads USB
diagnostics. Its `flash` subcommand requires an explicit UF2 SHA-256, GPIO, and
protocol, validates the flash range (excluding NVS), and matches the runtime and
BOOTSEL drive to the same physical USB port. Obtain operator confirmation before
flashing. `--bootloader-port <port>` is an explicit recovery option when a failed
application is already in BOOTSEL and no runtime tty exists.

Unexpected task stalls are monitored by a watchdog. One watchdog failure
restarts the firmware; repeated failures enter BOOTSEL so the enclosure need not
be opened for USB recovery. A healthy 30-second run resets the failure counter.
Intentional `reboot` and `reboot bootsel` remain available when no operation is busy.
