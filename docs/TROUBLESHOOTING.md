# Troubleshooting

## No USB device

```sh
lsusb -d cafe:4001   # running firmware
lsusb -d 2e8a:0003   # BOOTSEL mode
```

If neither appears, try another USB data cable or port.

## Permission denied

Before installation, use the stable `/dev/serial/by-id/` path and your
distribution's serial-device group, or temporarily test as root. After
installation, reconnect the Pico and check `/dev/cec-controller`.

## Protocol probe timeout

Open the serial console at 115200 baud and enter `tv protocol`. It must return
a line beginning with:

```text
CECCTRL/1 OK command=protocol
```

If it says `Unknown command`, flash the current UF2.

## CEC line is low

Unplug HDMI from the TV immediately. Recheck male-plug orientation, continuity
to pins 13/17, shorts, and insulation. An idle-high line with occasional brief
dips is normal.

## Logical address is `0x0f`

The Pico could not claim a playback address. Check the wiring and power-cycle
the Pico with the TV awake. One startup `tx noack` is normal—it confirms that
the selected logical address was free.

## Physical-address detection fails

Set the intended GPU connector or TV input in `/etc/cec-controller.conf`:

```ini
connector=card1-HDMI-A-1
# or
physical_address=3.0.0.0
```

## Automatic control fails

```sh
systemctl status cec-controller-boot.service cec-controller-sleep.service
journalctl -b -u cec-controller-boot.service -u cec-controller-sleep.service
```

CEC errors are logged but intentionally never block PC sleep or shutdown.

For an issue report, include TV/OS details plus `cecctl protocol`,
`cecctl detect`, `cecctl status`, and firmware `show stats cec` output. Remove
USB serial numbers from logs before posting.
