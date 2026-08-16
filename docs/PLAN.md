# Implementation plan

Each phase ends in a focused commit. A phase is complete only when its
acceptance gate has been met; physical validation cannot be replaced by a
successful compile.

## Phase 0 — repository and design baseline

- create a private project repository while retaining `pico-cec` history;
- record the separate-video-and-CEC topology;
- document electrical constraints and fallback boundaries;
- define firmware, host, and validation phases.

Acceptance gate: the private remote exists and the design documents are
committed and pushed.

## Phase 1 — reproducible RP2040 baseline

- pin all upstream dependencies through Git submodules;
- default the build to the original Raspberry Pi Pico;
- choose and document a CEC GPIO;
- build an unmodified-capability baseline with USB CDC and manual physical
  address configuration;
- document flashing and a minimal serial smoke test.

Acceptance gate: a clean checkout can produce a UF2, and the exact build and
smoke-test commands are documented.

## Phase 2 — purpose-built firmware protocol

- remove host-facing features that the couch-PC controller does not require;
- expose a small, versioned USB command protocol;
- implement idempotent `on`, `standby`, `status`, and configuration commands;
- implement Samsung-friendly wake ordering, bounded retries, ACK reporting,
  and useful diagnostics;
- preserve a low-level raw-frame command for bring-up.

Acceptance gate: automated host-side protocol tests pass and a UF2 builds from
a clean tree.

## Phase 3 — Bazzite host integration

- implement one small native command-line client with no resident daemon;
- discover the real GPU HDMI connector and parse its EDID physical address;
- add systemd boot, shutdown, suspend, and resume hooks;
- make device discovery and failure behavior deterministic;
- package installation and removal without scattering files.

Acceptance gate: host tests pass, systemd units verify, and idle steady-state
resource use is zero because no service remains running.

## Phase 4 — hardware bring-up

- continuity-check HDMI pins 13 and 17 before connecting either device;
- verify that no other cut-cable conductors can short together;
- validate CEC electrical idle level, logical-address allocation, ACKs, wake,
  input selection, and standby in that order;
- record the actual GPU input physical address and TV-specific timing.

Acceptance gate: repeated suspend/resume and shutdown/boot cycles control the
TV without the remote, with results recorded in a test log.

## Phase 5 — conditional fallbacks and extras

Fallbacks are tried one at a time and only when a logged observation justifies
them:

1. adjust command order, delay, or retry policy;
2. test whether the spare input requires protected/current-limited HDMI +5 V
   presence signaling;
3. if cross-input `Active Source` is rejected, build a properly terminated
   inline CEC tap on the real video connection;
4. only if necessary, add level-shifted DDC access;
5. after the primary goal is stable, add volume or other controls.

Acceptance gate: the smallest reliable hardware design is retained and unused
experiments are removed from the supported configuration.
