# Roadmap

## Completed baseline

- separate normal-video and CEC-only HDMI topology;
- reproducible original-Pico firmware build;
- versioned USB commands for wake, standby, status, and activation;
- small Linux client with EDID physical-address discovery and cache;
- systemd/udev lifecycle integration with no resident daemon;
- native protocol, EDID, serial, and CEC-sequence tests;
- physical validation on an older Samsung Anynet+ TV;
- successful standby, wake, and automatic selection of the PC's HDMI input.

The verified circuit uses only CEC and CEC ground. DDC, HDMI +5 V, and an
inline video-cable tap were not needed.

## Next release milestone

- validate repeated boot/shutdown and suspend/resume cycles on the Bazzite
  couch PC;
- collect compatibility reports from additional TVs and Linux distributions;
- publish checksummed firmware and Linux client release artifacts;
- tag the first public release after target-PC lifecycle validation.

## Future platform work

- Windows host client and power-event integration;
- macOS host client and sleep/wake integration;
- examples for non-systemd Linux init systems;
- packages or community recipes where they reduce setup friction.

## Optional features

After the power lifecycle is stable across more TVs:

- volume and mute commands;
- configurable CEC command ordering and wake delay;
- support for additional RP2040 boards with documented pin maps;
- an electrically reviewed inline adapter for TVs without a spare HDMI input.

DDC access or HDMI +5 V presence signaling will be considered only for a
specific, reproducible compatibility problem and must use suitable protection
and level shifting.
