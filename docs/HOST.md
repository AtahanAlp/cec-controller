# Linux integration

`cecctl` is a small C program with no daemon mode. systemd runs it only when
the PC boots, shuts down, suspends, or resumes.

## Support

Automatic setup requires Linux with systemd, udev, and DRM connector data in
`/sys/class/drm`. This covers Bazzite and most Fedora, Ubuntu, Debian, and
Arch-based desktops. Other init systems can call `cecctl` from equivalent
hooks.

## Build and manually test

```sh
./tools/build-host

device=/dev/serial/by-id/usb-Couch_CEC_Couch_CEC_Controller_*-if00
build/host/cecctl --config /nonexistent --device $device protocol
build/host/cecctl --config /nonexistent --device $device detect
build/host/cecctl --config /nonexistent --device $device status
build/host/cecctl --config /nonexistent --device $device standby
build/host/cecctl --config /nonexistent --device $device on
```

Run these with the TV awake and exactly one Pico attached. `detect` reads the
CEC physical address from the EDID of the GPU's HDMI connection. The last good
address is cached because some TVs hide EDID while sleeping.

## Install

Only after manual standby and wake work:

```sh
sudo ./tools/install-host
```

The installer adds:

- `/usr/local/bin/cecctl`;
- `/etc/cec-controller.conf`;
- one udev rule creating `/dev/cec-controller`;
- two systemd oneshot units;
- `/var/lib/cec-controller/physical-address` when an address is cached.

It enables but does not start automatic control. Reconnect the Pico, then run:

```sh
cecctl detect
cecctl status
sudo systemctl start cec-controller-boot.service
```

## Configuration

Usually auto-detection is enough. If several HDMI displays are active, edit
`/etc/cec-controller.conf`:

```ini
connector=card1-HDMI-A-1
```

If EDID has no CEC address, set the PC's TV input directly:

```ini
physical_address=3.0.0.0
```

Use the input carrying video, not the spare CEC-only input.

## Check or remove

```sh
systemctl status cec-controller-boot.service cec-controller-sleep.service
journalctl -b -u cec-controller-boot.service -u cec-controller-sleep.service
sudo ./tools/install-host --uninstall
```

Uninstall retains the configuration and cached address so a later reinstall
does not discard user choices.

## Confirmed operations (client 0.2 / protocol 2)

Update firmware and host together. The client waits for a terminal result with
the matching request and controller session. It returns 0 for a verified result,
5 for a controller error, 6 for confirmed power with unconfirmed input selection,
and 4 for serial/version/ownership failures.

The client serializes access across manual commands and lifecycle services.
Firmware retains operation ownership until confirmed state or a bounded failure;
rapid off/on commands must not overlap. A power operation may take up to 30 s
for wake or 10 s for standby, so install the updated service timeout values too.
TVs without usable status feedback return explicit failures.

Cached addresses are bound to a DRM connector and EDID identity. Version-1 cache
files are ignored; a fresh successful EDID detection repopulates the version-2
cache. If EDID is unavailable, configure the PC's physical input explicitly.

`tools/validate-tv.py` records protocol/status feedback without changing PC power.
The explicit `--power-cycles` option exercises sequential TV standby/wake and
requires operator confirmation. A report records power verification and routing
evidence separately; the visible TV input still needs operator observation.
