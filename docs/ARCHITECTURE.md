# Architecture

## Responsibilities

The host owns policy. It knows whether the PC is starting, resuming,
suspending, or shutting down, and it can inspect the EDID of the actual GPU
connector. It invokes a short-lived command-line client only at those events.

The Pico owns CEC timing and bus state. It allocates a Playback Device logical
address, transmits frames, receives replies, stores minimal configuration, and
returns a bounded success or failure result over USB.

The TV is the CEC root device at physical address `0.0.0.0`. The Pico must
advertise the physical address of the PC's real video input even though its CEC
wire initially enters the TV through a different input.

## Why DDC is excluded initially

DDC on the CEC-only cable would read the EDID for the spare TV input, not the
input carrying PC video. It would therefore produce the wrong CEC physical
address. It is also a 5 V I2C bus, while RP2040 GPIO is specified for 3.3 V
operation and is not 5 V tolerant.

The host-side alternative is both safer and semantically correct:

1. identify the connected DRM HDMI output under `/sys/class/drm`;
2. read that output's binary `edid` attribute;
3. extract the CEC physical address from the HDMI Vendor-Specific Data Block;
4. configure the Pico through USB before sending activation commands.

A manual physical address remains available for bring-up and as a deterministic
fallback.

## Initial CEC transactions

Assuming Playback Device 1 logical address `4` and PC input physical address
`1.0.0.0`, the important frames are:

| Purpose | Frame | Meaning |
| --- | --- | --- |
| advertise | `4f:84:10:00:04` | Report PA `1.0.0.0`, Playback Device |
| wake | `40:04` | Image View On, Playback 1 to TV |
| select | `4f:82:10:00` | Active Source at `1.0.0.0` |
| standby | `40:36` | Standby, Playback 1 to TV |
| query | `40:8f` | Give Device Power Status |

The logical-address nibble is not hard-coded in the final protocol; it reflects
the address successfully allocated by the Pico. Likewise, `1.0.0.0` is an
example and must be replaced with the real GPU input's address.

## Host lifecycle

```text
boot/resume
    -> own serial device and probe protocol/session
    -> resolve PC input address
    -> request wake with unique ID
    -> Image View On, then confirm stable reported On
    -> advertise physical address and Active Source
    -> record power and routing verification separately

suspend/shutdown
    -> own serial device and request Standby
    -> confirm stable reported Standby or reach the bounded deadline
    -> log terminal result, then allow host transition to continue
```

CEC failure must never prevent the PC from suspending or shutting down. It
should produce a concise journal entry and a useful client exit status.

## USB interface direction

The firmware retains the upstream CDC shell so the electrical setup can be
tested with raw frames. Normal operation uses a deliberately small, versioned
command set. Commands must be idempotent
where possible, newline framed for easy diagnosis, and bounded by host and
device timeouts.

The host locates the controller by stable USB identity rather than assuming a
particular `/dev/ttyACM*` number. Installation will use standard systemd and
udev locations; the source tree remains self-contained.

The udev rule creates `/dev/cec-controller` only when the CDC VID/PID and the
firmware manufacturer/product strings all match. `cecctl` probes `CECCTRL/2`
before issuing any control operation, so a wrong or incompatible serial device
cannot be treated as this controller.

The Linux host caches only the last valid physical address. This is necessary because
some TVs remove hot-plug detect and EDID in standby. It does not cache TV power
state or run a reconciler; lifecycle events remain the sole policy inputs.

## Operation ownership and recovery

The CDC parser owns shell access and USB output. A static control worker owns
one high-level operation at a time while the CEC task continues to transmit and
receive. New power requests receive `busy` rather than becoming deferred effects.
Internal request generations and deadlines prevent late completions from being
reused. Received messages are dispatched before queued transmissions, with a
bounded ISR receive queue and incomplete-frame recovery.

Host processes take a device lock and exclusive tty access before configuration
or traffic. USB IDs and the controller boot identity correlate terminal results;
CEC itself has no matching transaction ID for power reports. TV sender, report
length/value, receive time, and repeated observations constrain confirmation.
The address cache stores connector and EDID identity; ambiguous live outputs
cannot be replaced silently with cached data.

See [protocol details](PROTOCOL.md) and [troubleshooting](TROUBLESHOOTING.md).
