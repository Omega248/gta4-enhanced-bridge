# GTA IV Enhanced

A path-traced enhancement of Grand Theft Auto IV built on **NVIDIA RTX Remix**, extending xoxor4d's
[GTA IV RTX Remix Compatibility Mod](https://github.com/xoxor4d/gta4-rtx) with player features,
performance work on the Remix runtime and an automated testing toolkit.

> Private build. All credit for the Compatibility Mod goes to xoxor4d and contributors (see `UPSTREAM_README.md`).
> RTX Remix is by NVIDIA. Not for redistribution. Requires a legal copy of GTA IV.

---

## Features

### In game
| Key | Feature |
|---|---|
| **F7** | **GTA IV Enhanced menu** – Photo, Saves, HUD, World (time / weather / police), City life, More |
| **F8** | **Photo mode** – free camera independent of Niko |
| **F5** | **Save anywhere** – opens the game's own save screen (all slots) outside missions |
| **F4** | Full developer / RTX settings menu (from the Compatibility Mod) |

**Photo mode controls:** WASD move · mouse / arrows look · Q/E down/up · Shift fast · Ctrl slow · Z/X zoom ·
`[` `]` tilt · T freeze time · H toggle HUD · N hide Niko · R reset · **Space take photo**
(JPEG, HUD-free, saved to `Pictures\GTA IV Enhanced`).

**HUD modes:** Full · Minimal (radar only when driving or wanted) · Off (cinematic).

**Living city:** traffic and crowds follow the time of day *and* the weather – rain empties the
streets and cafés, storms clear them, fog thins traffic, sunny afternoons bring people out.
Changes ease in over ~10 s. Traffic and pedestrian amounts are adjustable.

**Handling presets:** `HANDLING - REALISTIC.bat` / `HANDLING - CLASSIC.bat` in the game folder.

Settings are stored in `GTA4Enhanced.ini` in the game folder (keys are Windows virtual-key codes).

### Rendering / performance (Remix runtime patches – `remix-runtime/patches`)
| Patch | What it does |
|---|---|
| 0001 | Per-frame CPU zone profiler + counters → `rtx-remix/gta4e_profile.txt` |
| 0002 | Keep the merged-BLAS bucket cache when an instance is removed (no full rebuild every frame) |
| 0003 | Texture residency planning every Nth frame |
| 0004 | Separate merged-BLAS buckets for recently changing instances |
| 0005–0006 | Draw-path diagnostics |
| 0007 | Optional: drop same-frame duplicate submissions |
| 0008 | All of the above switchable **live** via `rtx.gta4e.*` options (no restart) |
| 0009 | Experimental: skin GTA IV vertex-shader meshes (peds, vehicle parts) inside Remix instead of vertex capture (`rtx.gta4e.vsSkinning`, off by default) |
| 0010 | Fix: reset Remix's skinning command counter after each flush (avoids doubled queue submissions) |
| 0011 | Skinned meshes keep the shader in their geometry hash |

Recommended Remix settings applied by this build: DLSS Quality, object anti-culling off
(it caused black blotches from stale instances).

### Testing toolkit
- **Bridge** (`claude_bridge.*`): file-based command channel in
  `%USERPROFILE%\Downloads\GTA4-Enhanced-Setup\bridge` (override with `GTA4_BRIDGE_DIR`). Nothing listens on the network.
  - `inbox\<id>.cmd` (`cmd=<name>` + `key=value` lines) → `outbox\<id>.json`
  - `status.json` – live state twice a second
  - `session_log.jsonl` – passive telemetry, `crash_log.txt` – crash location
- **Scripts:** `script file=<name> report=<name>` runs `bridge\scripts\<name>.txt`
  (`wait`, `wait_idle` – waits for Remix asset loading, `measure`, `screenshot`, any command).
  `scripts\autorun.txt` runs once after the game loads.
- **No-click start:** `tools/autostart.sh on|off` (presses Enter on the game's own launcher screen).
- **Tools:** `tools/abtest.py` (interleaved in-game A/B tests), `tools/shotdiff.py` (screenshot diff),
  `tools/report.py`, `tools/TESTING.md`.

Main bridge commands: `status`, `teleport`, `time`, `weather`, `hud`, `god`, `calm`, `camera` / `camera_off`,
`spawn_car`, `remix` / `remix_raw <option>=<value>`, `screenshot`, `measure`, `wait_idle`, `hide`, `ff_cat`,
`photo on=`, `quicksave`, `living_city on=`, `exit_game`.

---

## Install
1. GTA IV (Complete Edition) with FusionFix and RTX Remix set up as described in `UPSTREAM_README.md`.
2. Copy `a_gta4-rtx.asi` (from the **Build** workflow artifact) into the game folder.
3. Optional: copy the patched runtime `d3d9.dll` (from the **Remix runtime (gta4e)** workflow artifact) to `.trex\`.
4. Start the game; press **F7** for the Enhanced menu.

## Build
- **Mod:** `generate-buildfiles_vs22.bat` → Visual Studio 2022 → Release (x86). CI: `.github/workflows` "Build".
- **Remix runtime:** CI workflow "Remix runtime (gta4e)" clones xoxor4d's `dxvk-remix` branch
  `game/gta4_atmos10_nr2`, applies `remix-runtime/patches/*.patch` in order and builds with meson/ninja
  (incremental, cached).

## Credits
- xoxor4d – GTA IV RTX Remix Compatibility Mod and the GTA IV dxvk-remix branch
- NVIDIA – RTX Remix
- ThirteenAG and contributors – FusionFix
