# Third-party notices

This project is derived from
[gkoh/pico-cec](https://github.com/gkoh/pico-cec). The original MIT copyright
notice is retained in the root [LICENSE](LICENSE) file and in relevant source
files.

Pinned Git submodules retain their own license files:

| Component | License file |
| --- | --- |
| Raspberry Pi Pico SDK | `pico-sdk/LICENSE.TXT` (BSD 3-Clause) |
| Raspberry Pi FreeRTOS Kernel | `FreeRTOS-Kernel/LICENSE.md` (MIT) |
| tcli | `tcli/LICENSE` (MIT) |
| BOSL2 OpenSCAD library | `openscad/BOSL2/LICENSE` |

The vendored `crc/` sources come from
[gityf/crc](https://github.com/gityf/crc). Most carry Apache License 2.0
notices; the complete license is in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt).
`crc/crc/crc64.c` contains its own BSD 3-Clause notice from Redis.

Some Pico SDK example-derived files retain BSD or TinyUSB MIT notices directly
in their headers. Those notices apply to the corresponding files.
