# Game-aware scene submission (design, 2026-10-18)

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
