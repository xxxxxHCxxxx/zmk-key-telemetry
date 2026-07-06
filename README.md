# zmk-key-telemetry

ZMK module that streams live key-position and layer-state changes over a
custom BLE GATT service, for a companion app that visualizes the keymap in
real time (highlighting pressed keys, following momentary layer holds).

Only builds on the split-central board (the half that owns the BLE
connection to the host and already sees both halves' key events combined).

## Enabling

Add this repo to your `zmk-config`'s `west.yml` as a project (same pattern
as any other ZMK module), then add `-DCONFIG_ZMK_KEY_TELEMETRY=y` to the
central-side entry's `cmake-args` in `build.yaml`.

## BLE protocol

- Service UUID: `d53e3c3a-511c-499f-9708-c752305d41b4`
- Characteristic UUID: `67f1d079-5c05-4d80-87a1-0f4a9d0c4aee` (notify only)

Every notification is a fixed 3-byte frame:

| Byte | Meaning |
| --- | --- |
| 0 | frame type: `0x01` = position state, `0x02` = layer state |
| 1 | index: physical key position (0-based) for type `0x01`, layer index for type `0x02` |
| 2 | state: `0x00` = released/inactive, `0x01` = pressed/active |

No length prefix or escaping is needed - each BLE notification carries
exactly one frame.

## Why not ZMK Studio?

ZMK Studio's RPC protocol (`zmk-studio-messages`) is a keymap *editing*
protocol; its only notifications are `lock_state_changed` and
`unsaved_changes_status_changed`. It has no real-time key-press or
active-layer event stream, so it can't drive a live visualizer. This
module exposes the internal `zmk_position_state_changed` /
`zmk_layer_state_changed` events (which already carry exactly this
information) over its own minimal service instead.
