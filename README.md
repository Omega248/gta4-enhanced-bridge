# GTA IV Enhanced – Compatibility Mod + Claude bridge (private build)

Private build of xoxor4d's [GTAIV RTX Remix Compatibility Mod](https://github.com/xoxor4d/gta4-rtx)
(upstream commit d6bd170, v1.5.2) with one added module: `src/gta4/modules/claude_bridge.*`.
All credit for the Compatibility Mod goes to xoxor4d and contributors (see UPSTREAM_README.md).
Not for redistribution.

## Bridge
A file-based command channel in `%USERPROFILE%\Downloads\GTA4-Enhanced-Setup\bridge`
(override with env `GTA4_BRIDGE_DIR`). Nothing listens on the network.

- `inbox\<id>.cmd` – `key=value` lines, first `cmd=<name>`
- `outbox\<id>.json` – result
- `status.json` – live state (position, zone, time, weather, health, in_car), twice a second
- `screens\<id>.bmp` – screenshots

Commands: `ping`, `status`, `teleport x y z [heading]`, `time hour minute [freeze]`, `release_time`,
`weather id`, `release_weather`, `hud on`, `god on`, `freeze on`, `camera x y z (rx ry rz | look_x look_y look_z) [fov]`,
`camera_off`, `spawn_car model [warp]`, `remix rtx.<option>=<value> …`, `screenshot [hide_hud delay_frames]`,
`capture [settle_frames capture_frames]` (temporarily disables replacement assets, triggers the RTX Remix capture hotkey, restores).
