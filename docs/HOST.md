# Bazzite host integration

`cecctl` is a small C executable linked only to glibc. It has no background
mode. Two systemd oneshot services invoke it at lifecycle boundaries, so no
client process, private memory, or client CPU use remains at steady state.

## Build and test

From the repository root:

```sh
./tools/build-host
```

The Podman build compiles with warnings as errors, runs EDID and reply-parser
tests, exercises a complete command exchange through a pseudo-terminal, and
checks both unit files with `systemd-analyze verify`. The resulting executable
is `build/host/cecctl`.

The client is intentionally dependency-free beyond glibc. Bazzite recommends
avoiding `rpm-ostree` layering when a standalone command will work. On the
rpm-ostree layout, `/usr/local` is machine-local state backed by `/var`, so the
installer can use the conventional `/usr/local/bin` location without altering
the OS image.

## Address discovery

For `on`, the client selects the address in this order:

1. `physical_address=` from `/etc/cec-controller.conf`;
2. the HDMI VSDB in the EDID of `connector=`, if configured;
3. the only connected `HDMI-A` connector with a valid CEC address;
4. the last valid address cached in
   `/var/lib/cec-controller/physical-address`.

The cache matters on real TVs: hot-plug detect can fall and make EDID vanish
while the display is in standby. The first discovery should therefore be done
with the TV awake. If more than one connected HDMI connector contains a CEC
address, auto-detection fails safely and asks for a connector name.

Examples:

```sh
build/host/cecctl --config /nonexistent --sysfs-root /sys/class/drm detect
build/host/cecctl --config /nonexistent --physical-address 1.0.0.0 detect
```

After installation, omit the executable path and config override:

```sh
cecctl detect
cecctl protocol
cecctl on
cecctl status
cecctl standby
```

`on` sends the discovered address and activation request as one versioned USB
command. All device opens and replies have finite deadlines. Exit code `0`
means success, `2` is command-line misuse, `3` is address/configuration
failure, `4` is USB transport failure, and `5` is a controller-reported CEC
failure.

## Install on Bazzite

Build first, then install from the checkout:

```sh
./tools/build-host
sudo ./tools/install-host
```

The installer places exactly these managed files:

- `/usr/local/bin/cecctl`;
- `/etc/udev/rules.d/70-cec-controller.rules`;
- `/etc/systemd/system/cec-controller-boot.service`;
- `/etc/systemd/system/cec-controller-sleep.service`;
- `/etc/cec-controller.conf`, only when it does not already exist;
- `/var/lib/cec-controller/`, for the last-known physical address.

It enables but does not start the services. This prevents installation from
transmitting CEC before the cable has passed the Phase 4 continuity checks.
After hardware bring-up:

```sh
cecctl detect
cecctl on
sudo systemctl start cec-controller-boot.service
```

The boot service runs `on` once and remains active without a process; normal
systemd shutdown stops it and invokes `standby`. The sleep service is tied to
`sleep.target`: `ExecStart` runs `standby` before suspend/hibernate, and
`ExecStop` runs `on` after resume. Leading `-` markers in the unit commands
make CEC failure visible in the journal without blocking a PC power
transition.

Useful diagnostics:

```sh
systemctl status cec-controller-boot.service cec-controller-sleep.service
journalctl -u cec-controller-boot.service -u cec-controller-sleep.service
```

Uninstall program integration while retaining the user-edited configuration
and cached address:

```sh
sudo ./tools/install-host --uninstall
```

## Design references

- [Linux CEC physical-address documentation](https://docs.kernel.org/userspace-api/media/cec/cec-ioc-adap-g-phys-addr.html)
  defines the `a.b.c.d` hierarchy and confirms that a source reads its address
  from the sink EDID.
- [Linux HDMI-CEC administration guide](https://docs.kernel.org/admin-guide/media/cec.html)
  demonstrates obtaining an address from a DRM connector's `edid` sysfs
  attribute.
- [Linux CEC transmit documentation](https://docs.kernel.org/userspace-api/media/cec/cec-ioc-receive.html)
  records the important standby case where HPD falls and the EDID disappears.
- [systemd.special](https://man7.org/linux/man-pages/man7/systemd.special.7.html)
  specifies the combined `sleep.target` oneshot pattern used here, including
  `RemainAfterExit`, `StopWhenUnneeded`, `ExecStart`, and `ExecStop`.
- [Bazzite software guidance](https://docs.bazzite.gg/Installing_and_Managing_Software/software-intro/)
  recommends standalone approaches over system package layering where
  possible.
- [rpm-ostree treefile reference](https://coreos.github.io/rpm-ostree/treefile/)
  documents the default `/usr/local` mapping to persistent machine-local
  `/var` storage.
