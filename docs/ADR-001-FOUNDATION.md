# ADR-001: Foundation for the GTA IV remaster (2026-10-18)

## Status
Accepted. To be revisited if an Xbox 360 copy of GTA IV (disc image + Title Update 8) becomes available,
or if the public recompilation projects reach full gameplay.

## Context
Goal: a modern remaster (native 64-bit where possible, hardware ray tracing, DLSS, PBR/4K materials,
parallax interiors, stable 60 fps), preserving missions, physics, audio and saves.

What exists on this PC (verified):
- GTA IV Complete Edition PC, `GTAIV.exe` 1.2.0.59, 32-bit (PE machine 0x14C), large-address-aware.
- FusionFix, RTX Remix (64-bit runtime + 32-bit bridge), xoxor4d's GTA IV compatibility mod, and our
  fork of both (`Omega248/gta4-enhanced-bridge`): 13 runtime patches, player features, test tooling.
- Path tracing, DLSS Super Resolution, Ray Reconstruction and Frame Generation are running today;
  ~88,600 PBR/upscaled material overrides and 527 mesh overrides are installed.
- No Xbox 360 game files on any drive.

## Options evaluated
| Option | Evidence | Verdict |
|---|---|---|
| A. Port the PC exe to 64-bit | No source; RAGE resources store 32-bit pointers; every plugin is 32-bit | Rejected: no feasible route |
| B. Static recompilation of the Xbox 360 build | [OZORDI/LibertyRecomp](https://github.com/OZORDI/LibertyRecomp): "early development, NOT meant for public use", GPU pipeline in progress, no licence. [luisxl15/GTA-IV-RECOMP-XBOX-360](https://github.com/luisxl15/GTA-IV-RECOMP-XBOX-360): boots and renders on D3D12, but world collision never loads and saves are rejected. Both need the user's Xbox 360 copy (+TU8). rexglue SDK (D3D12/Vulkan, Clang 18+) is early | Not selected now: blocked on game files; gameplay incomplete |
| C. B plus our own renderer | Needs B to work first; the Xenos runtime has no scene representation for ray tracing, and Remix cannot hook it (no D3D9) | Deferred behind B |
| D. New engine around the assets | Years of work to rebuild missions, physics, AI and scripting | Rejected |
| E. Current stack: PC game + Remix (forked runtime) + mod | Works end to end today, with path tracing, DLSS SR/RR/FG and PBR materials. Limits: 32-bit game process, 32-to-64 bridge, CPU and GPU budgets | **Selected** |

## Decision
Build on option E. Remove its bottlenecks inside our runtime fork and mod, rather than replacing a
working path-traced renderer and full gameplay with an incomplete recompilation:
- persistent identity-tagged instances (patch 0013 + mod tags);
- per-pass GPU profiling (patch 0012) to drive GPU cuts;
- frame overlap (2 frames in flight, applied);
- an asset pipeline for materials and textures.

Track B as a research spike: if the user supplies an Xbox 360 copy, build luisxl15/rexglue and record what
works (collision, missions, saves). Consider switching only when gameplay parity is shown.

## Consequences
- **Native 64-bit executable:** not delivered. The game logic stays 32-bit, while Remix (renderer, path
  tracing, DLSS) is already a native 64-bit process. This is the main gap from the brief and is stated as a
  limitation, not hidden.
- **Renderer backend:** Vulkan (inside Remix). Hardware ray tracing and DLSS are verified running today.
- **Risks:**
  - The bridge's per-call cost.
  - The single-threaded Remix CS thread.
  - The GPU budget (~27 ms at Star Junction when CPU-relieved).
  - Remix replacement-material loading (~40 ms/frame for the first 4-5 minutes after launch).
- **Gameplay preservation:** the original executable runs unmodified apart from hooks, so missions,
  physics, audio and saves are the original code.
