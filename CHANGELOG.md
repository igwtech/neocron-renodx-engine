# Changelog

## [0.1.0] — 2026-05-09

Initial release. Tier 4 (engine shader injection) milestones 1 and 2
shipped.

### Milestone 1 — pixel-shader identification (probe)

Hooked `init_pipeline` to CRC32-tag every PS Neocron creates and dump
the bytecode to `Z:/tmp/neocron_ps_<crc>.dxbc` for offline analysis.
Built a `(vs_crc, ps_crc, z_state) → draws` histogram so we could
correlate which PS pairs with which VS in 3D-world rendering.

Findings:

- `0x2ccf5eb7` (496 B) = **world.ps** — lit world geometry
  (`samplerAlbedo`, `samplerLightmap`, `colorCorrection` per CTAB).
- `0x96f566cb` (416 B) = generic textured PS — used by skinned-mesh
  characters, alpha-blended world overlays (signs, decals), AND HUD
  elements. The "do everything textured no lighting" PS.
- `0xeb1b9a91` (448 B) = always-on overlay PS — pairs with the
  one-draw-per-frame overlay VS `0x92e9b61d` (cursor / damage flash).

Vertex shader outputs decoded from `world.vs` (CRC `0x5925dce1`)
bytecode: `oPos`, `oFog`, `oD0`, `oT0` = TEXCOORD0 = albedo UV,
`oT1` = TEXCOORD1 = lightmap UV.

### Milestone 2 — HLSL replacement compiled at addon load

- `d3dcompiler_47.dll` loaded dynamically via `LoadLibrary` /
  `GetProcAddress`. The Microsoft DLL ships with the launcher
  `neocron-renodx` addon — it must be a real Microsoft build, not
  Wine's reimplementation, or HLSL compilation fails.
- HLSL source for the replacement world.ps is embedded in the addon as
  a string literal and compiled to ps_2_0 bytecode at
  `DLL_PROCESS_ATTACH` time via `D3DCompile`. The compiled blob lives
  in a `std::vector<uint8_t>` for the lifetime of the addon.
- `addon_event::create_pipeline` (which fires BEFORE the underlying API
  creates the pipeline object — unlike `init_pipeline`) is the right
  hook point for substitution. The handler receives mutable
  `pipeline_subobject` data; pointing the PS subobject's
  `desc->code` / `code_size` at our blob and returning `true` tells
  ReShade we modified the description.

The v0.1 replacement shader is functionally close to vanilla
(`albedo * lightmap * 2.0`) with a 1.05x green push as a "this is our
shader" marker. Validated empirically by an interim "red tint" build
that produced unmistakably warm-tinted world geometry while leaving
HUD overlays and characters untouched (proving the substitution scope
is precisely `world.ps` and nothing else).

### Skipped / TODO for v0.2

- Real normal-mapped lighting: needs sampler-2 binding (modify
  `SetTexture` interception to look up `<name>_n.dds` siblings) plus
  HLSL math for tangent-space normal sampling + Phong against a sun
  direction or the engine's dynamic light state. Multi-day work
  blocked on the AI normal-map corpus (separate project
  `_normal_pipeline/`).
- Extension to `mesh.ps` (CRC `0x96f566cb`) for normal-mapped
  characters and skinned meshes. Same approach but different
  replacement HLSL.
- `world.vs` extension to compute and forward a tangent basis (current
  VS only outputs position + UVs + diffuse; tangent-space normal
  mapping needs the basis at the PS).
