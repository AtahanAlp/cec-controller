# Contributing

Contributions, TV compatibility reports, documentation fixes, and host clients
for additional operating systems are welcome.

## Before opening a change

- Keep the default hardware path limited to CEC pin 13 and ground pin 17.
- Do not add direct RP2040 connections to HDMI +5 V or DDC.
- Keep every USB and CEC operation bounded; TV failure must never block host
  suspend or shutdown.
- Preserve the no-daemon design unless a feature has a demonstrated need for a
  resident process.
- Separate verified behavior from expected compatibility.

For a substantial new feature, open an issue first so protocol and hardware
implications can be discussed before implementation.

## Development setup

Clone all submodules and run the reproducible checks:

```sh
git clone --recurse-submodules https://github.com/AtahanAlp/cec-controller.git
cd cec-controller
./tools/test-firmware
./tools/build-host
./tools/build-firmware
```

Use `clang-format` with the repository's `.clang-format` file for C/C++
changes. Do not reformat imported code under `crc/` or Git submodules.

## Pull requests

- Keep commits focused and use concise imperative subjects.
- Add or update tests for behavior changes.
- Update user documentation when commands, wiring, or installation changes.
- State which TV, MCU, host OS, and topology were tested.
- Never include USB serial numbers, private logs, or unrelated generated files.

## Compatibility reports

A useful report includes:

- TV manufacturer and model;
- RP2040 board and selected GPIO;
- host OS and version;
- CEC-only and video HDMI input numbers;
- output from `cecctl protocol`, `cecctl detect`, and `cecctl status`;
- whether standby, wake, and input selection each worked;
- `show stats cec` output when a command failed.

If the exact TV model is unavailable, say so and include the CEC trade name
(Anynet+, Bravia Sync, Simplink, and so on).

## Licensing

By contributing, you agree that your contribution is distributed under the
project's MIT license. Retain copyright and license notices in imported or
derived files.
