# USB command protocol

The controller exposes one USB CDC ACM interface. Human-readable shell output
may appear around command replies, but every machine reply is one newline-
terminated line beginning with `CECCTRL/1`. Host software must ignore all
other lines and require the expected protocol version and command name.

The RP2040 unique ID is used as the USB serial number, so Linux creates a
stable entry under `/dev/serial/by-id/`. The firmware no longer exposes the
upstream USB HID keyboard interface.

## Commands

### Protocol probe

```text
> tv protocol
CECCTRL/1 OK command=protocol firmware=<git-version>
```

The host should issue this before its first control command after opening the
device.

### Wake and select the PC input

```text
> tv on
CECCTRL/1 OK command=on attempts=1
```

The physical address must already be set. `tv on` sends, in order:

1. broadcast `Report Physical Address`;
2. directed `Image View On`, retried up to three times when necessary;
3. a 1.5 second Samsung wake delay;
4. broadcast `Active Source`.

Broadcast frames are successful when their transmission completes; CEC does
not use a directed ACK for them. Success requires the TV to ACK `Image View
On`. The full operation is bounded even when the bus is held low.

### Standby

```text
> tv standby
CECCTRL/1 OK command=standby attempts=1
```

Directed `Standby` is attempted at most twice. Repeating the command is safe.

### Power status

```text
> tv status
CECCTRL/1 OK command=status power=on value=0
```

Power values are:

| Value | Name |
| --- | --- |
| 0 | `on` |
| 1 | `standby` |
| 2 | `transitioning_on` |
| 3 | `transitioning_standby` |

The controller requires a directed ACK for `Give Device Power Status`, then
waits up to two seconds for the TV's `Report Power Status` reply.

## Errors

Errors use this shape:

```text
CECCTRL/1 ERR command=<command> code=<code> attempts=<number>
```

The defined codes are:

| Code | Meaning |
| --- | --- |
| `invalid_physical_address` | Address is unset or has an invalid CEC hierarchy |
| `no_ack` | The TV did not acknowledge a directed command |
| `tx_timeout` | The bus did not become idle or frame transmission timed out |
| `response_timeout` | A status query was ACKed but no status reply arrived |
| `unavailable` | The firmware command path was not ready |

The host may log and retry a wake command once, but it must never block PC
suspend or shutdown indefinitely because of a CEC error.

## Runtime configuration

The existing shell command updates the running physical address immediately:

```text
set config physical_address 1000
```

`save` persists the current configuration. The automatic Bazzite integration
will normally derive the real GPU input address and set it at runtime, while a
saved value remains useful for manual bring-up.

The compact NVS configuration format is version 4. Older upstream keymap-based
NVS records are deliberately ignored and fall back to defaults because this
controller has no HID keymap feature.

## Diagnostic raw send

Raw frame transmission remains available:

```text
> send 0 04
CECCTRL/1 OK command=send result=ack
```

For destination `f` (broadcast), either `ack` or `no_ack` means the frame was
transmitted. Raw send is for bring-up and should not be used by normal host
lifecycle integration.
