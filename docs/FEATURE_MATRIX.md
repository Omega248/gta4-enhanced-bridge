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
| Real 60 fps in heavy scenes | Blocked (GPU) | Star Junction at full quality is GPU-bound at ~32-36 real fps on the RTX 5070 Ti (GPU fence wait ~31 ms). Only big toggle: secondary bounces off +21% but loses almost all indirect light (rejected). Volumetrics +3.5%, PSR +2%, POM/RTXDI/NRC ~0 (`gpumap3`). Lighter scenes reach ~60 (spawn 64 fps) |
| Frame overlap (2 frames in flight) | Verified | +12.5% static fps, 4 alternating launches (overlap A/B, 2026-10-18) |
| Persistent identity-tagged instances | Implemented but unverified | Static A/B (`persist_134415`): draws to Remix 3,084 -> ~1,620, CS -4..5 ms, render thread -5 ms, fps -1.8% (noise: scene is GPU-bound); static visuals OK (0.7% pixels differ vs 0.3% base-to-base). Driving visuals/LOD swaps unverified (AI route not repeatable). Off by default (`retain_k = 0`) |
| Per-pass GPU timing | Implemented but unverified | b17: top-level InjectRTX timed every frame (13.8 ms GPU at spawn, 64 fps); nested sub-passes still under-sampled |
| Scene probe (draw/entity statistics) | Verified | `probe_083757.jsonl`: 2,840 draws, 861 entities, 350 models |
| PBR / upscaled materials | Verified | ~88,600 material overrides in installed USD layers |
| Parallax / displacement materials | Verified | `rtx.displacement.mode = 2`; factor reduced to 1 after "too much depth" feedback |
| Parallax interiors (window depth) | Not investigated | |
| 4K texture pipeline (repeatable) | Investigated | Existing tools in `GTA4-Enhanced-Setup` (upscale, material rules); not re-audited this session |
| Startup material-loading cost | Verified | Parallel DDS header prefetch at launch (mod, `asset_prefetch.cpp`): loading done ~40 s after control instead of 4-5 min; findAsset 4.6 -> 0.1-0.2 ms/call (`loadtime_*.jsonl`, `rtx-remix/logs/gta4e_prefetch.log`) |
| Ultrawide / high refresh / HDR | Not investigated | |
| Graphics presets (Low...Path Tracing) | Not investigated | |
| Gameplay preservation | Verified (by design) | Original executable and scripts, hooks only; no mission regression suite yet |
| Automated test runner | Verified | `tools/gtatest.py` launch / run / exit / analyse, used for all runs above |

## GPU feature cost at Star Junction (CPU relieved, interleaved, noise ~1%) - `gpumap3_*.jsonl`
| Change | Real fps | Visual impact |
|---|---|---|
| Secondary bounces off | +20.9% | Severe: ~80% of pixels change, scene loses indirect light (rejected) |
| Volumetrics off | +3.5% | Small in clear weather (1.3% pixels); matters in fog/rain |
| PSR (reflection/transmission) off | +1.9% | Glass/reflection quality |
| Displacement off / 16 steps, RTXDI 8 samples, lighter NRC, PSR depth 4 | ~0% | - |
