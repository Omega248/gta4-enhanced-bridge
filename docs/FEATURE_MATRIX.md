# Feature matrix (2026-10-18)
Statuses: Not investigated / Investigated / In development / Implemented but unverified / Verified / Blocked.
"Verified" means observed in the running game, with evidence noted.

| Feature | Status | Evidence / notes |
|---|---|---|
| Native 64-bit game executable | Blocked | PC exe is 32-bit, no source (ADR-001). Recompilation route needs an Xbox 360 copy, which is not on this PC |
| 64-bit renderer process | Verified | Remix runtime (`.trex/d3d9.dll`, NvRemixBridge) is x64 |
| Modern backend (Vulkan) | Verified | Remix/DXVK Vulkan, `remix-dxvk.log` |
| Hardware path tracing | Verified | Remix path tracer running, per-pass GPU zones (Gbuffer/Integrate raytracing) in `gta4e_profile.txt` |
| DLSS Super Resolution | Verified | `rtx.upscalerType = 1`, `qualityDLSS` in `rtx.conf`; ray reconstruction pass in GPU zones |
| DLSS Ray Reconstruction | Verified | "Ray Reconstruction" GPU zone, `rtx.enableRayReconstruction = True` |
| DLSS Frame Generation | Verified | `rtx.dlfg.enable`; overlay shows ~3x displayed fps; DLFG zones present |
| Real 60 fps in heavy scenes | In development | Star Junction 36-46 real fps; limits: Remix CS thread, bridge thread, render thread, GPU ~27 ms |
| Frame overlap (2 frames in flight) | Verified | +12.5% static fps, 4 alternating launches (overlap A/B, 2026-10-18) |
| Persistent identity-tagged instances | Implemented but unverified | Tags 637 draws/frame, skips ~1,300, CS -9 ms; fps gain not yet measured cleanly; visuals unchecked |
| Per-pass GPU timing | Implemented but unverified | b16: G lines present but under-sampled; b17 fix deployed, startup check pending |
| Scene probe (draw/entity statistics) | Verified | `probe_083757.jsonl`: 2,840 draws, 861 entities, 350 models |
| PBR / upscaled materials | Verified | ~88,600 material overrides in installed USD layers |
| Parallax / displacement materials | Verified | `rtx.displacement.mode = 2`; factor reduced to 1 after "too much depth" feedback |
| Parallax interiors (window depth) | Not investigated | |
| 4K texture pipeline (repeatable) | Investigated | Existing tools in `GTA4-Enhanced-Setup` (upscale, material rules); not re-audited this session |
| Startup material-loading cost | Investigated | ~40 ms/frame for 4-5 min after launch (`persist_092553.jsonl`) |
| Ultrawide / high refresh / HDR | Not investigated | |
| Graphics presets (Low...Path Tracing) | Not investigated | |
| Gameplay preservation | Verified (by design) | Original executable and scripts, hooks only; no mission regression suite yet |
| Automated test runner | Verified | `tools/gtatest.py` launch / run / exit / analyse, used for all runs above |
