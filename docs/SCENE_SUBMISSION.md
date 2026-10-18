# Game-aware scene submission (design, 2026-10-18)

> **Revised after architect review (2026-10-18), see "Revision" at the end.** Short version: persistent,
> identity-tagged instances on the existing D3D9 path first; Remix API meshes only later, for what D3D9
> cannot express; and measure/cut GPU cost in parallel, because the GPU is the next wall.

## Goal
Real 60 fps (no frame generation) in heavy scenes with path tracing, and correct materials, by
having the mod tell Remix what the scene *is* instead of Remix rebuilding it from ~3,000 D3D9
draws and ~29,000 state calls every frame.

## Measured starting point (Star Junction, b14 runtime, 2-frame overlap)
- ~37 real fps. Not GPU bound (render scale 0.67 -> 0.5, frame gen on/off, 1 vs 2 bounces: all +-2%).
- CPU stages, per frame: Remix CS thread saturated (~25-30 ms: submitDrawState, determineMaterialData,
  processDrawCallState, BLAS/instance bookkeeping); bridge server receive thread ~20 ms
  (internalPrepareDraw, hashing); game render thread ~17 ms (mod per-draw logic ~8 ms, bridge
  setters ~5 ms).
- Instance identity by hash is fragile: keeping instances >1 frame (rtx.numFramesToKeepInstances)
  gave ~+20% but caused flicker on moving parts; draw retain caused flicker on VS-animated parts.

## Architecture: hybrid, category by category
D3D9 path stays for everything not yet migrated (peds, particles, water, UI, sky, effects).
Migrated categories are submitted through the Remix API from the game's own scene data and their
D3D9 draws are suppressed in the mod's renderer hook.

Per migrated object:
- **Mesh**: created once per (model, LOD, geometry) with `CreateMesh`, from the game's
  `rmcDrawable -> grmLodGroup -> grmModel -> geometry` vertex/index data (converted to
  `remixapi_HardcodedVertex`). Destroyed when the model streams out.
- **Material**: created once per shader instance with `CreateMaterial`. Type comes from the game's
  shader/effect (glass, decal, emissive, vegetation, water...) and collision material, not from
  texture-hash lists in rtx.conf.
- **Instance**: per visible entity per frame `DrawInstance(mesh, transform)`; phase 2 makes static
  instances persistent (runtime patch) so only changes cross the bridge.
- **Identity**: game object/model IDs, never vertex-data hashes, so instance matching is exact
  (fixes the flicker class above).

## Facts checked
- Bridge (client d3d9.dll and NvRemixBridge.exe) carries CreateMesh, DestroyMesh, CreateMaterial,
  DestroyMaterial, DrawInstance, lights, SetConfigVariable. It does NOT carry CreateTexture,
  SetupCamera or CreateMeshBatched.
- The mod already uses CreateMesh/CreateMaterial/DrawInstance (debug lines/circles, remix_api.cpp)
  and CreateLight, so the submission path works end to end.
- The mod already models CEntity, rmcDrawable, grmModel, grmLodGroup, geometry, CBaseModelInfo,
  shader effects and grcTexturePC (game/structs.hpp).
- Runtime: API meshes get synthetic geometry hashes (`hack_getNextGeomHash`); API materials take
  texture *paths*, plus a fork "0x<hash>" pseudo-path that only resolves API-uploaded textures.
- Installed remaster content: ~88,600 material overrides vs 527 mesh overrides (USD layers).
  Material replacements are keyed by the legacy material hash (albedo texture hash).

## Required runtime patches (our patch series, CI-built)
1. **Texture by D3D9 hash**: let an API material reference a texture the game already uploaded
   through D3D9 (resolve "0x<hash>" against the D3D9 texture cache), so no texture data is resent.
2. **Replacement material for API surfaces**: when an API surface's albedo is a D3D9 texture hash,
   apply the replacement material registered for that legacy hash (keeps the 88k overrides).
3. (later) **Caller-provided geometry hash** for the 527 mesh replacements.
4. (phase 2) **Persistent instances**: create/update/destroy instead of per-frame DrawInstance.

## Phases
0. **Scene probe (no render change)**: per frame at Star Junction, enumerate what the game draws:
   entities, model/LOD, geometry counts, shader/effect names, texture hashes, D3D9 draws per entity.
   Output to status.json. Answers: how many static entities, how many unique meshes/materials,
   which categories cover most draws, and whether texture hashes match what Remix sees.
1. **Proof of concept**: static world buildings in one area via API, their D3D9 draws suppressed;
   patches 1+2. Success = same image (shotdiff vs D3D9 path, replacement materials applied) and
   lower CS/bridge time. Measured with tools/gtatest.py + rebench_analyze.py.
2. Persistent instances; extend to props, vehicles (incl. wheels), then skinned peds.
3. Semantic materials from game shader types; retire rtx.conf texture-hash lists for migrated
   categories.

## Phase 0 results (2026-10-18, Star Junction static view, probe build v15)
Per frame: 2,840 indexed draws, of which 2,151 (76%) come from the static-world entity path
(rmcDrawable::Draw with a CEntity). 861 entities, 350 unique models, 1,475 unique entity
geometries (vb/ib/offset/start/prims), ~950 textures. 1,224 entity draws are on "Fixed" entities.
Shader mix: gta_default 49% of draws, gta_spec 11%, gta_trees 9%, emissive variants ~13%,
normal/normal_spec ~11%, peds 3%, vehicles/glass/decals small. While driving: 2,100-3,200 draws,
620-790 entities. The entity-type bits decoded from m_dwFlags2 are not meaningful for buildings
(uniform histogram) - do not rely on them.
Implication: migrating the static-world path covers ~3/4 of all draws with ~860 instances of
~350 models per frame; with persistent instances the steady-state per-frame traffic for that
part drops from ~2,150 draws + their state calls to the instances that change.

## Open questions / risks
- Exact texture hash Remix uses for D3D9 textures, and whether the mod can obtain it per texture
  (compute the same hash in the mod, or have the runtime report it).
- Streaming: textures/models stream in and out; handle lifetimes must follow the game's.
- Culling: Remix needs off-screen geometry for reflections/GI; the D3D9 path relies on the mod's
  anti-culling. With API submission the mod decides the set (radius-based), which is a benefit
  but needs a budget.
- Per-frame DrawInstance traffic before phase 2 (one bridge command per visible entity).
- Bridge API limits: no CreateTexture/SetupCamera across the bridge (camera still comes from D3D9).

## Revision (architect review + relieved GPU map, 2026-10-18)

### Corrections
- "Not GPU bound" was wrong: those tests ran while the CS thread was saturated. With the CPU relieved
  (retain k=8, keep 10) Star Junction runs ~36 fps with the GPU busy ~27 ms/frame (dxvk-queue fence
  waits). Resolution scale 0.5: -2% (noise); pathMaxBounces 1: +2%; enableSecondaryBounces off: +18%;
  frame generation off: +7%; hiding all static world draws: +31% (GPU 21 ms). Most GPU cost is not
  resolution dependent.
- API draws go through processDrawCallState every frame (rtx_scene_manager.cpp submitExternalDraw), so
  API submission alone saves ~0-2 ms of CS time. The gain comes from persistence, which does not need
  the API.
- API instances lose the per-draw extras (render states 42/149/150/169/177/210-220), legacy-hash rules
  (150/220) and get a fixed REPEAT sampler; new fields would need a 32-bit bridge build as well.
- Instance matching is not exact today: L1 identity hash (geometry+material+transform), else L2 tracking
  hash + spatial nearest neighbour (rtx_draw_call_tracker.cpp findOrCreateReplacementInstance). GC uses
  one global lifetime (rtx.numFramesToKeepInstances) - why keep=5 ghosted moving objects.

### Plan B: identity-tagged persistent instances (D3D9 path)
- Mod: static-world draws (entity known from rmcDrawable::Draw) set render state 221/222 = game
  identity (entity, LOD, geometry index) + lease length; vertex-animated shaders (trees/wind, water,
  animated UVs) are not tagged.
- Runtime patch: identity read in the D3D9 draw-state capture (next to the existing 42/149/... reads);
  tagged draws look up their ReplacementInstance by identity (exact, no L2 search); GC keeps a tagged
  instance alive while its lease is valid, independent of numFramesToKeepInstances (stays 1 for
  everything else, so moving objects never ghost); identity map cleared with the tracker.
- Mod then skips re-sending tagged, unchanged static draws, renewing the lease every K frames.
- Risk to handle: DrawCallCache may re-bind an untouched geometry entry to a similar draw
  (rtx_draw_call_cache.cpp) - pin entries owned by leased instances.

### GPU track
- Runtime patch 0012: GPU time per named GPU pass from Vulkan timestamps ("G" lines in
  gta4e_profile.txt), since Tracy is compiled out. Decide cuts from measured per-pass costs
  (BLAS builds, NRC, ray reconstruction, frame generation, volumetrics, secondary bounces).

### Phase 1 success criterion (Star Junction vista + scripted drive, interleaved vs baseline)
CS busy <= 15 ms, bridge thread <= 12 ms, render thread <= 14 ms, GPU <= 15 ms; real fps median >= 55,
p99 frame <= 25 ms while streaming; replacement-material hits equal to baseline; screenshot diff under
threshold at 3 fixed views; no stale instances after teleport / LOD swap.
