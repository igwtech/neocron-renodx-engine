# Changelog

## [0.3.0] — 2026-05-10

Tier 4 milestone 3 PoC — actual normal-mapped Phong replacement of
`world.ps`. Architecturally complete; visual effect intentionally
subtle pending m4 (per-texture lookup).

### What's new
- Loads a single AI-generated brick normal map at addon startup via
  `stb_image` + raw D3D9 `CreateTexture` / `LockRect` (no d3dx
  dependency, no PNG decoder built-in to Wine needed).
- Hooks `addon_event::draw` / `draw_indexed` — when the substituted
  world.ps is bound + Z-test enabled, calls `dev->SetTexture(5, normal)`
  + sampler-state setup before each draw.
- Replacement HLSL samples `samplerNormal : register(s5)` at tiled UVs,
  decodes RGB → tangent normal, computes Lambertian against a hardcoded
  sun direction, modulates `albedo × lightmap × 2`.

### Diagnostic findings shipped as in-source comments
- **Sampler index matters.** Initial attempt bound to `s2`; output
  appeared as solid pink-white. Switched to `s5`. Suspected ReShade /
  DXVK / game state-tracking using `s2` for something internal —
  didn't fully diagnose; just moved off.
- **ReShade post-processing masks the bump.** Toddyhancer tonemap
  shifts blue-violet (typical normal map output) toward pink/white.
  Toggle ReShade Effects OFF (default key) to see raw m3 output.
- **Single normal for all surfaces is a known limitation.** This PoC
  binds ONE brick normal map for every world.ps draw. Surfaces that
  aren't brick (carpet, concrete, tile) get the brick pattern
  inappropriately. m4 deals with this via per-texture lookup against
  the AI-generated `_normal_pipeline/` corpus (~3000 sibling normals).

### Build / deploy
- `bin/neocron-renodx-engine.addon32` (~12 MB MinGW i686 cross-compiled,
  static-linked, includes `stb_image.h` PNG decoder).
- `assets/default_normal.png` is now part of the addon's `files`
  (deployed as `~/Neocron2/neocron_default_normal.png`).
- `expects:` keeps `d3d9.dll` + `d3dcompiler_47.dll` from
  `neocron-renodx`; `requires:` chain unchanged.

## [0.2.0] — 2026-05-09

Replacement HLSL extended with **luminance-bump fake normal mapping**.
Self-contained — does not depend on the per-texture AI normal map
corpus (that's m3 next session).

How it works:

```
albedo  = sample(samplerAlbedo, uv)
luma_c  = luminance(albedo.rgb)
luma_r  = luminance(sample(samplerAlbedo, uv + (0.005, 0)))   // small U offset
luma_u  = luminance(sample(samplerAlbedo, uv + (0, 0.005)))   // small V offset

n_ts    = normalise( (luma_c - luma_r) * 3,
                     (luma_c - luma_u) * 3,
                     1.0 )                                    // pseudo-normal

NdotL   = saturate(dot(n_ts, SUN_TS))                         // hardcoded sun
light   = 0.65 + 0.35 * NdotL                                 // ambient + diffuse

return albedo * lightmap * 2 * light
```

Effect: surfaces with high-contrast albedo texture (panel seams, brick
grout, carpet weave, scuff marks) get visible bump-style highlights
and shadows in the direction of a hardcoded sun. Flat surfaces stay
roughly vanilla.

ps_2_0 instruction count: ~14 (well under the 64 limit). Performance
hit: 2 extra texture taps per world pixel (~5% on Polaris).

## [0.1.1] — 2026-05-09

Launcher distribution polish — no functional changes.

- `addon.json` now fetches `neocron-renodx-engine.addon32` from the
  v0.1.0 GitHub release artifact. End users install via the launcher
  by pasting the repo URL — no mingw-w64 build step required.
- `bin/` stays in `.gitignore` (binaries live in the GitHub release).

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
