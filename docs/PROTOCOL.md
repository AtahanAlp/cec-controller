# USB command protocol, version 2

The Pico exposes one USB CDC ACM interface. Commands are newline terminated;
replies begin with `CECCTRL/2`. Diagnostic shell prompts and logs are not machine
replies. Firmware and host must both support version 2. Version 1 reported power
command delivery without confirming the TV state and is deliberately rejected
before the version-2 client issues a power command.

## Probe and command identity

```text
> tv protocol
CECCTRL/2 OK command=protocol firmware=<version> session=<16-hex-digits> on_ms=30000 standby_ms=10000 status_ms=3000 gpio=1 ready=1 busy=0
```

`session` is a random boot identity, not the board's USB serial number. A changed
session means the controller restarted and previous operation results are no
longer available. `ready` means a logical address has been allocated; it does not
prove that the TV is awake. `busy` means another control operation owns the
controller. The GPIO is a build setting: the generic repository default is
GP11, while the enclosed couch-PC controller is configured for GP1.

The client generates a random nonzero 64-bit request ID. Machine commands are:

```text
tv on 3000 0123456789abcdef
tv standby 0123456789abcdf0
tv status 0123456789abcdf1
tv result 0123456789abcdef
```

For manual diagnosis the ID can be omitted. `tv on` can also omit the physical
address if a valid address has already been set. The physical address must
identify the PC's video input, not the spare CEC-only input. `0000`, `ffff`, and
invalid hierarchies are rejected.

One accepted operation owns the controller until a terminal result or deadline.
The parser, CEC receiver, and USB task remain available during that wait. New
operations receive an error with their own ID and `code=busy`; they are not
queued for future power effects. Raw sends, save, configuration, and reboot
commands are also rejected while an operation is active.

The eight most recent terminal results are retained in RAM for this boot.
Repeating a retained ID with the same command/parameters returns that result;
reusing it with different parameters returns `id_conflict`. `tv result <id>`
returns the original terminal line without transmitting anything to the TV.
Unknown IDs return `unknown_request`; active operations return `busy`.
Do not reissue an uncertain power operation to recover a missing result: use the
result lookup. Retention is bounded and IDs are scoped to a controller boot.

## Confirmed power operations

Wake sends `Image View On` first. The controller then queries TV power status
until two consecutive valid reports say `on`, or its deadline expires.
Transitions and ACKs do not count as completed wake. Standby similarly requires
two reports of `standby`. Reports are separated by a 500 ms polling interval.
Retries are capped and share the same overall deadline. Wake can send at most
three power requests; standby at most two, with at least two seconds between
retries. A known opposite transition is allowed to settle before another retry.

After confirmed wake, the controller allows a settling interval, advertises its
physical address, and announces `Active Source`, with at most one repeat. A
valid TV routing change/information report received during the operation is
recorded separately as routing evidence, including reports that precede the
final announcement. A later conflicting source or TV route invalidates it. The Pico's own advertisement is not input confirmation.
Another source becoming active stops the repeat.

Example terminal result:

```text
CECCTRL/2 OK command=standby id=<id> session=<session> code=ok power=standby value=1 verified=1 source=none routing=unconfirmed attempts=1 elapsed_ms=1350 phase=power
```

Every accepted operation includes `id`, `session`, `code`, `power`, `value`,
`verified`, `source`, `routing`, `attempts`, `elapsed_ms`, and `phase` in its
terminal line. `verified` describes TV power feedback. `source` is `none` or
`announced`; `routing` is `confirmed` or `unconfirmed`. These describe work/evidence
for this operation: `source=none` on a status query does not mean the TV has no
active input. These are observations
through CEC, not proof from a physical screen sensor.

If the TV reports `on` but provides no routing evidence, the result is explicit:

```text
CECCTRL/2 ERR command=on id=<id> session=<session> code=input_unconfirmed power=on value=0 verified=1 source=announced routing=unconfirmed attempts=1 elapsed_ms=2350 phase=activate
```

The client prints `result=partial` and exits 6. This means power was confirmed
but input selection was not fully verified. Do not interpret it as a failed wake
or silently promote it to fully verified success. A TV that stops answering
power queries after standby cannot produce a confirmed standby result; silence
ends in an error rather than presumed success.

`tv status <id>` queries current reported power without changing it:

| Value | Power |
| --- | --- |
| 0 | `on` |
| 1 | `standby` |
| 2 | `transitioning_on` |
| 3 | `transitioning_standby` |
| 255 | `unknown` (error results only) |

## Errors and deadlines

| Code | Meaning |
| --- | --- |
| `busy` | Another operation owns the controller; no new operation was accepted |
| `invalid_physical_address` | Input address is missing or invalid |
| `no_ack` | The TV did not acknowledge the directed request/query |
| `tx_timeout` | CEC transmission could not finish in time |
| `response_timeout` | No usable TV status feedback arrived before the deadline |
| `transition_timeout` | TV feedback arrived but did not establish the target state |
| `unsupported_feedback` | The TV returned Feature Abort for the power query |
| `unavailable` | Logical address, USB connection, or operation path was unavailable |
| `input_unconfirmed` | TV power was confirmed but input activation was not verified |
| `id_conflict` | A retained request ID was reused with different parameters |
| `unknown_request` | The requested result is not retained for this boot |

Rejected requests have at least `command`, `id`, `session`, and `code`.
They do not claim power verification.

Firmware deadlines are 30 s for wake/activation, 10 s for standby, and 3 s for
status, including readiness and retries. A query's timeout covers transmission
and its response together. Expired internal transmissions cannot execute later;
late task notifications cannot complete another transmission.

The host owns the serial device exclusively before configuring or flushing it.
It matches both request ID and boot session, rejects incomplete/malformed
replies, and bounds the whole invocation to 40 s for wake and 20 s for standby.
On an uncertain reply timeout it may query the retained result once, without
repeating power effects. The service limits are 45 s for wake and 25 s for
standby, leaving room for the client's terminal result and cleanup. Errors are
logged and never hold PC suspend or shutdown indefinitely.

## Configuration and raw diagnosis

```text
show cec
show stats cec
set config physical_address 3000
save
```

Configuration is validated before applying. Save is allowed only while the bus
is idle. GPIO selection remains a build setting and is shown by `show cec` and
the protocol probe. Older valid NVS version-4 configuration is retained; invalid
stored values fall back to defaults.

Raw `send <destination> <opcode> [operands...]` is an explicit diagnostic tool.
It reports frame delivery, not completion of a TV power action. Directed frames
require byte-level ACKs; a broadcast reports `result=complete` after successful
transmission. Raw sends do not bypass the active-operation gate.
