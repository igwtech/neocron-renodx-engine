# Changelog

## [0.5.1] — 2026-05-10

Diagnostics-first release. v0.4.9 left scenes mostly black, and we
couldn't tell which channel of `albedo × vcol × lightmap × cc` was
zero without rebuilding the binary every iteration. v0.5.1 exposes
EVERY input as a slider in the overlay AND adds 7 new debug viz
modes so the user can isolate the broken channel themselves.

### New ImGui controls (overlay → Add-ons → Neocron RenoDX Engine)
- `vcol -> 1.0 mix` — fade game's vcol toward white (0=use vcol, 1=force 1)
- `lightmap -> 1.0 mix` — same for lightmap
- `colorCorrection override` — slider replacing game's c4.x; -1=use game's
- `albedo boost` — scalar multiplier on albedo before everything else

### Debug viz expanded to 11 modes (was 4)
- 0 Lit, 1 Raw normal, 2 NdotL gray, 3 Albedo (per-tex)
- **4 Vanilla raw** — final formula output, no math
- **5 Albedo channel** — pure tex2D(s0)
- **6 Lightmap channel** — pure tex2D(s1)
- **7 vcol channel** — interpolant after fallback
- **8 cc.x grayscale** — game's colorCorrection.x as gray
- **9 vcol after mix** — vcol post slider
- **10 Lightmap after mix** — lm post slider

### Conservative defaults
- `lightmap-driven` 1.0 → 0.0 (fixed sun by default; lightmap-gradient
  produced wrong-direction sun in some zones)
- `light_min` 0.20 → 0.60 (less darkening when NdotL is low)
- `light_max` 1.50 → 1.40
- `bump_amp` 5.0 → 4.0
- `spec_strength` 0.20 → 0.15
- `rim_strength` 0.30 → 0.00 (was the vaseline source)

### How to debug a scene
1. Set Debug viz = 8 (cc.x gray) — if scene is black, game's c4 is zero
2. Set Debug viz = 6 (lightmap) — confirm lightmap is non-zero
3. Set Debug viz = 5 (albedo) — confirm sampler 0 isn't reading black
4. Set Debug viz = 4 (vanilla raw) — see final formula output
5. Use override sliders to bypass any broken channel.

## [0.4.9] — 2026-05-10

Hotfix for v0.4.8: black screen at login menu. The unified replacement
read `vcol` from `TEXCOORD2` (matching the disassembled game shader's
`dcl t2`), but on Wine/DXVK an interpolant unwritten by the VS reads as
`(0,0,0,0)` instead of D3D9 reference's `(1,1,1,1)`. The game's `world.vs`
writes its diffuse to `oD0` (COLOR0), not `oT2` — so for world / menu
draws our shader was multiplying everything by zero → black.

### Defensive fallbacks
- `vcol` now reads from BOTH `TEXCOORD2` AND `COLOR0`, picks whichever
  is non-zero (defaults to `(1,1,1,1)`).
- `colorCorrection` falls back to `1.0` when `c4.x` is zero.
- `lightmap` falls back to `(1,1,1)` when the bound sampler is all-zero.

These defensive defaults match what D3D9 reference behaviour appears to
provide on Windows when interpolants / constants are unset.

## [0.4.8] — 2026-05-10

Reverse-engineered the game's exact pixel-shader formula via
D3DDisassemble (a tiny Wine-runnable tool was built for the purpose).
All 4 dumped game pixel shaders (`0x96f566cb`, `0xbea29c90`,
`0xcc8904f8`, `0xeb1b9a91`) turned out to disassemble to byte-identical
7-instruction code:

```
output.rgb = albedo.rgb * vcol.rgb * lightmap.rgb * colorCorrection.x
output.a   = albedo.a   * vcol.a
```

So NC2's renderer effectively has ONE pixel shader for everything, and
v0.4.8 ships ONE unified replacement that mirrors this vanilla formula
exactly when `BUMP_ON=0`. UI / overlays / signs are now byte-identical
to the game's original output (they were corrupted in v0.4.7).

### Critical bugs fixed
- **vcol was being read from `COLOR0`; the game ACTUALLY uses
  `TEXCOORD2`.** That single mismatch explains the gray-rectangle UI
  in v0.4.7 — we were reading garbage interpolant data.
- `colorCorrection` (game's c4) is now read by our shader; missing it
  was washing out colors.
- `lightmap` is sampled in mesh.ps too (HUD elements bind a 1×1 white,
  but the game's shader still reads from s1).
- **`bind_normal_for_draw` now ALWAYS pushes constants** when our
  shader is bound, including HUD draws (z-test off). Without this,
  the shader read stale `BUMP_ON=1` from a previous world.ps draw and
  tried to bump-map the HUD with whatever was last in sampler 5.
- Lightmap gradient sign inverted (`+grad`, not `-grad`).
- Spec/rim modulated by lightmap so unlit areas don't glow.
- Default rim strength = 0 (was 0.3 → caused vaseline look).
- Default spec strength halved.
- Normal Y flip toggle (DeepBump uses Y-down) — checkbox in overlay,
  default ON.
- Alpha preservation: returns `vanilla.a = albedo.a * vcol.a`.

### Coverage
Substitution list expanded to all 5 known game PS CRCs (3 world
variants + mesh + overlay). All five route through the same unified
replacement. HUD safety: per-tex hashing skipped when z-test off, and
HUD textures don't match the index anyway, so BUMP_ON=0 → vanilla
output.

### Tooling
`tools/disasm.cpp` (new) — i686-w64-mingw32 + d3dcompiler_47.dll +
Wine, disassembles any `.dxbc` to ASM. Used to reverse all game PSes.

## [0.4.7] — 2026-05-10

Cover the two extra world.ps **variants** (`0xbea29c90` and
`0xcc8904f8` — same sampler layout as the main world.ps, different
fog / color-correction state) by substituting them with the same
runtime-compiled HLSL replacement. NPCs / cops / barrels / items
running through mesh.ps already work after restart of v0.4.6.

### Changed
- `WORLD_PS_CRCS[]` array replaces the single `WORLD_PS_CRC` constant.
- Both `on_create_pipeline` and `on_init_pipeline` iterate the array.
- Dump filter excludes all known CRCs (3 world variants + mesh.ps).

## [0.4.6] — 2026-05-10

Scene-reactive lighting (lightmap-derived sun direction) + view-
dependent specular & rim. The bumps were "static" before because we
shaded against a hardcoded sun direction; now they respond to the
level's actual baked lighting AND to camera motion.

### How
- **Lightmap-gradient sun**: for every pixel, sample the lightmap at
  4 nearby UVs, take the luminance gradient, use `-∇L` as the local
  light direction in tangent space. Bumps now point toward whatever
  the level designer baked as bright (neon signs, windows, etc.).
- **VPOS-derived view direction**: upgraded both replacement shaders
  to ps_3_0 (DXVK supports trivially) so we can read the screen
  position via the VPOS semantic. Combined with the screen size
  (refreshed every frame from `GetViewport`), gives a per-pixel
  approximation of the view ray in tangent space.
- **Half-vector specular**: Phong-style highlights using the lightmap-
  driven `L` and the VPOS-driven `V`. Highlights now slide across
  surfaces as you turn the camera.
- **Fresnel rim**: `pow(1 - N·V, 3)` puts a soft glow on grazing
  edges of bump features.

### New ImGui sliders
Scene-reactive lighting:
- Lightmap-driven (vs fixed sun) — 0..1 blend
- Gradient amp — 0..20 (4 default)
- Lightmap tap offset — 0.001..0.02

View-dependent specular:
- Specular strength — 0..2 (0.4 default)
- Specular shininess — 1..128 (16 default)
- Rim strength — 0..2 (0.3 default)

### New constants (PS register layout)
- c22 = (lm_driven, lm_amp, spec_strength, spec_shininess)
- c23 = (rim_strength, lm_tap_offset, screen_inv_w, screen_inv_h)

mesh.ps gets the same spec+rim treatment but no lightmap (it doesn't
sample one), so it falls back to the fixed sun direction for the L.

## [0.4.5] — 2026-05-10

Two fixes for v0.4.4 user feedback.

### Flat fallback (no more brick-on-everything)
Previously when an albedo had no per-texture normal match, the engine
bound the brick default normal to sampler 5 — every unmapped surface
got the brick pattern. v0.4.5 sets a `BUMP_ON` flag (c21.w) to 0 in
that case; the replacement HLSL skips sampler 5 entirely and outputs
vanilla `albedo * lightmap * 2.0`. Result: surfaces with a per-tex
hit get their AI normal; everything else looks exactly like vanilla.

### mesh.ps replacement (NPCs, items, decals)
The other game pixel shader, mesh.ps (CRC 0x96f566cb), is used by
NPCs, characters, items, signs, decals, AND HUD overlays. v0.4.5
substitutes mesh.ps with a parallel HLSL replacement that does the
same per-texture normal lookup at sampler 5 — but only when the
per-tex hash matches (HUD draws have Z-test off and game-side textures
that aren't in the index, so they pass through vanilla automatically
via the `BUMP_ON=0` path).

Both shaders share the same `c20`/`c21` pixel-shader constants and
the same ImGui overlay tab — no extra UI changes needed.

### Coverage roadmap
Stock corpus is still bricks-only. NPC / item normal generation needs
extracting `models.pak` (or pulling from `nc2-hd-textures/gfx/modeltextures/`
which is already 506 entries via the HD index when that addon is
installed).

## [0.4.4] — 2026-05-10

Switched from F2-F6/F11 hotkeys to ImGui sliders in the ReShade overlay.
F-keys collide with NC2's HUD shortcuts (F1-F12 reserved by the game).

### What's new
- Vendored ImGui v1.92.5-docking headers (`src/include/imgui.h`,
  `imconfig.h`) — ReShade SDK requires the docking-branch types.
- New addon overlay tab: **ReShade overlay (Home) → Add-ons → Neocron
  RenoDX Engine** has `Bump amp`, `Light min`, `Light max`, `Debug viz`
  combobox, `Reset` button, and live stats (world draws, per-tex
  hits, index hits/misses, lazy-loaded count, cache size).
- Removed `poll_hotkeys` + the F-key polling in `on_present`.

### Build change
`src/Makefile` already adds `-Iinclude`; no further changes needed,
ImGui is header-only.

## [0.4.3] — 2026-05-10

Runtime-tunable HLSL params + debug viz modes via in-game hotkeys.
DeepBump's normal output for low-contrast albedos has small XY tilt
(stddev ~7-16 / 255 for vanilla bricks), which a straight Lambertian
masks against the lightmap. v0.4.3 amplifies the XY tilt 5× by
default and exposes the amp + light range as runtime knobs so users
can tune by surface category.

### HLSL changes
- `n_ts.xy *= bump_amp;` before normalise (default amp = 5).
- Light range `lerp(min, max, NdotL)` instead of `0.4 + 0.6*NdotL`
  (default 0.20 .. 1.50 — wider contrast).
- 4 debug viz modes selectable via shader uniform: lit / raw normal /
  NdotL grayscale / albedo only.

### New pixel-shader constants (registers c20, c21)
- `c20.xyzw` = `(bump_amp, light_min, light_max, debug_mode)`
- `c21.xyz`  = sun direction in tangent space
- Picked above c0..c7 to avoid colliding with the game's world.ps
  color-correction constants.

### Hotkeys
- **F2** cycle debug viz: 0 lit → 1 raw normal → 2 NdotL gray → 3 albedo only
- **F3 / F4** bump amp -1 / +1 (range 0.5 .. 15)
- **F5 / F6** light range narrow / widen
- **F11** reset to defaults

Each hotkey logs the new state to `ReShade.log` for confirmation.

### v0.4.2 (skipped tag)
v0.4.2 only had the static BUMP_AMP=5 in the shader; rolled into v0.4.3
once the runtime tunability landed.

## [0.4.1] — 2026-05-10

Adds an engine-bundled **stock fallback index + normals** so per-texture
normals work even when nc2-hd-textures isn't installed. v0.4.1 ships
68 vanilla brick normals only — concrete / metal / glass etc. still fall
back to the brick default until subsequent vanilla batches are processed.

### What changed
- `assets/stock_brick_index.txt` (~2.5 KB, 67 hashes) — committed in
  repo, deployed to game root as `neocron_stock_index.txt`.
- `stock_brick_normals.tar.gz` (~61 MB, GitHub release artifact) —
  fetched + extracted to `gfx_normals_stock/` by the launcher.
- `engine_injector.cpp` now loads BOTH indexes (HD if present, then
  stock for non-overlapping hashes) into one map. Each entry's path
  has its corpus root pre-prepended at load time, so the draw-time
  lookup is corpus-agnostic.
- HD index entries take precedence on hash collision.

### Diagnostic line at addon load
```
renodx-engine: indexes loaded — HD <N> entries (root='gfx_normals\'),
stock <M> loaded / <K> kept (root='gfx_normals_stock\'); total map size = <N+K>
```

### Roadmap
- Future v0.4.x: expand stock corpus to concrete, metal, glass, doors,
  floor — same vanilla-extract + DeepBump pipeline.
- Engine binary stays unchanged across stock expansions; only the
  `assets/stock_*_index.txt` files and the release tarball grow.

## [0.4.0] — 2026-05-10

Tier 4 milestone 4 — **per-texture normal lookup**. Each world surface
now gets its own AI-generated normal map instead of the universal
brick from m3.

### Distribution change

The texture index (`neocron_texture_index.txt`) and the ~1.9 GB AI
normal corpus are now shipped by the **nc2-hd-textures** addon (since
they're paired with the HD albedos that produced them). This addon
ships only the engine binary + the brick fallback normal. With
nc2-hd-textures installed, per-texture normals work; without it, all
world surfaces fall back to the brick normal (still better than flat).

Stock vanilla world-only normals as engine-bundled fallback are TODO.

### How it works

1. `scripts/build-hash-index.py` walks the source corpus (`nc2-hd-textures/gfx/`),
   PAK-decompresses each `.dds`/`.bmp`, strips the file header, computes
   `(crc32(first_4 KB_pixels) << 32) | (width << 16) | height`, and
   matches it to the sibling `_n.png` from `_normal_pipeline/output/`.
   Output: `assets/texture_index.txt` (~115 KB, 2857 entries).

2. At addon load, the runtime parses the `.txt` into a `uint64_t →
   path` map.

3. Hashing happens **on first bind** in `bind_normal_for_draw`:
   `IDirect3DDevice9::GetTexture(0)` → QueryInterface to
   IDirect3DTexture9 → `LockRect(D3DLOCK_READONLY)` on the system-
   memory copy (POOL_MANAGED has one) → CRC32 of first 4 KB of raw
   pixel bytes. Each game texture is hashed exactly once over its
   lifetime; cached forever after.

   We tried using ReShade's `map_texture_region` / `unmap_texture_region`
   events first, but DXVK appears to release the staging buffer
   mapping by the time `unmap_texture_region` fires, so reading the
   captured pointer hit unmapped memory → SEGV. The on-bind LockRect
   approach is robust because the system-memory copy stays valid for
   the texture's whole lifetime.

4. `addon_event::draw[_indexed]` calls `dev->GetTexture(0, …)` to find
   the albedo bound for the current world.ps draw, looks up its hash,
   resolves the `_n.png` path, lazy-loads it via `stb_image` +
   `CreateTexture` (LRU-capped at 256 entries), and binds at sampler 5.

5. Hash unknown → falls back to the v0.3 brick normal (still useful as
   "something is bumpy" baseline).

### HLSL change

Tiling factor dropped from 4× to 1× — per-texture normals match the
albedo's UV layout 1:1.

### Distribution

- `assets/texture_index.txt` (115 KB) is shipped with the addon.
- The `~2900 _n.png` files (~1.9 GB) are NOT shipped — too large.
  Default search path is the dev machine's
  `Z:/home/javier/Documents/Projects/Neocron/_normal_pipeline/output/`.
  Override via env var `NEOCRON_NORMALS_DIR`. Future work: launcher-
  managed addon payload or DDS-compressed corpus distribution.

### Diagnostics

`reshade.log` reports every 1800 frames:
```
frames=N replacements=N world-draws=N(per-tex H) index-hits=H/misses=M
lazy-loaded=L cache-size=C default-normal=1
```
Use the `per-tex` count to verify per-texture binding rate.

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
