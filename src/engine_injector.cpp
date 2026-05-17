// neocron-renodx-engine — Tier 4 m4 (v0.4.0):
// per-texture normal-mapped lighting.
//
// Each game world texture (sampler 0 albedo) is fingerprinted via the
// SAME hash scheme the offline pipeline uses (CRC32 of first 4 KB of
// raw pixel data + width + height = 64-bit). The hash is looked up in
// a flat text index shipped with the addon (~115 KB, ~2900 entries).
// On hit, the matching `_n.png` from the AI normal corpus is lazily
// loaded as a D3D9 texture and bound at sampler 5 instead of the
// universal brick fallback.
//
// Hashing is done at `init_resource` time (initial_data is the raw
// upload buffer — same bytes the offline script reads from the DDS
// after stripping its 128-byte header). No D3D9 LockRect round-trip
// needed for hashing.
//
// Corpus location: hardcoded for the dev machine right now (see
// NORMAL_CORPUS_DIR below). Future work: launcher-managed addon
// payload or env-var override.
//
// What you should see in-game vs v0.3:
//   - Brick walls still get the brick normal (correct).
//   - Carpet, concrete, panelled walls now get THEIR own normals.
//   - Surfaces whose albedo isn't in the index fall back to the brick
//     normal (visible but wrong).
//   - Toggle ReShade Effects OFF (default Home key + button) to see
//     the raw m4 output — Toddyhancer mangles blue-violet normal data.

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
// ImGui must come BEFORE reshade.hpp so reshade_overlay.hpp picks it up.
// IMGUI_VERSION_NUM in our vendored header (1.92.5) must match what
// reshade_overlay.hpp expects (currently 19250).
#define ImTextureID ImU64
#include "imgui.h"
#include <reshade.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG   // v0.9: albedo ships as JPG (2GB release-asset limit)
#include "stb_image.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

namespace {

// ── tunables ─────────────────────────────────────────────────────────
// world.ps comes in multiple variants — each LOD / fog state / color-
// correction permutation has its own bytecode CRC. They all sample
// s0=albedo, s1=lightmap; we substitute every known variant with our
// single replacement (which adds normal sampling on s5 + lighting math).
// All known game pixel shader CRCs — all share the same 7-instruction
// `albedo * vcol * lightmap * colorCorrection.x` body (verified via
// D3DDisassemble). Single unified replacement covers all of them.
// HUD draws among these are protected because (a) z-test is off so
// bind_normal_for_draw doesn't bind a normal, (b) per-tex hash misses
// so BUMP_ON=0 → shader takes the vanilla path → output is byte-equal
// to the game's original.
constexpr uint32_t GAME_PS_CRCS[] = {
    0x2ccf5eb7u,  // world.ps primary
    0xbea29c90u,  // world.ps variant A
    0xcc8904f8u,  // world.ps variant B
    // mesh.ps + overlay.ps DELIBERATELY skipped — substituting them
    // triggers DXVK descriptor isolation (sampler 1 reads as gray
    // for unrelated reasons, even on world.ps draws). Cost: NPCs /
    // items / HUD don't get per-texture normals. Worth it for the
    // working baked lightmap on the world.
    // 0x96f566cbu,  // mesh.ps (NPCs, items, decals, HUD)
    // 0xeb1b9a91u,  // overlay.ps (cursor, damage flash)
};
constexpr uint32_t WORLD_PS_CRC = 0x2ccf5eb7u;  // legacy alias
constexpr UINT     NORMAL_SAMPLER        = 2;   // v0.5.5: back to s2 (contiguous with s0/s1) — sparse sampler use suspected of breaking DXVK state passthrough
constexpr UINT     ALBEDO_SAMPLER        = 0;   // v0.8: HD albedo replaces the game's clamped-512 s0 (only renodx bypasses the native clamp)
constexpr UINT     ORME_SAMPLER          = 3;   // v0.8: ORME (R=occ G=rough B=metal A=emis), contiguous after normal
constexpr const char* DEFAULT_NORMAL_FILE = "neocron_default_normal.png";
// Two indexes are loaded if present:
//   1. HD index (deployed by nc2-hd-textures addon, ~2900 entries hashed
//      against HD upscaled albedos, normals at gfx_normals/<rel>).
//   2. Stock index (shipped with this engine addon, hashed against
//      VANILLA game textures, normals at gfx_normals_stock/<rel>).
// HD entries take precedence on hash collision; stock fills in surfaces
// the user hasn't HD-installed yet (currently only bricks — see v0.4.1
// CHANGELOG).
constexpr const char* HD_INDEX_FILE     = "neocron_texture_index.txt";
constexpr const char* STOCK_INDEX_FILE  = "neocron_stock_index.txt";
constexpr const char* HD_CORPUS_DIR     = "gfx_pbr\\";          // v0.8: triplet corpus (albedo/normal/orme) deployed by nc2-hd-textures
constexpr const char* STOCK_CORPUS_DIR  = "gfx_normals_stock\\"; // legacy normal-only fallback (2-token lines)

// v0.8: 64 KiB (was 4 KiB). NC2 world textures are DXT — linear
// block-rows, no LockRect row padding — so offline-tight == runtime
// pitch and the proven match holds, while genuine hash collisions drop
// 69 -> 21. MUST stay in lockstep with build_pbr_index.py HASH_BYTES.
constexpr size_t HASH_SAMPLE_BYTES = 65536;
constexpr size_t NORMAL_CACHE_MAX  = 256;   // cap LRU; ~50 MB for typical 256² normals

// Pixel-shader constant registers — c0..c7 reserved for game's shaders.
constexpr UINT  PARAMS_PS_REG = 20;   // c20 = bump_amp, light_min, light_max, debug_mode
constexpr UINT  SUN_PS_REG    = 21;   // c21 = sun direction xyz + bump_on flag w
constexpr UINT  EXTRA_PS_REG  = 22;   // c22 = lm_driven, lm_amp, spec_strength, spec_shininess
constexpr UINT  EXTRA2_PS_REG = 23;   // c23 = rim_strength, lm_tap_offset, screen_inv_w, screen_inv_h
constexpr UINT  EXTRA3_PS_REG = 24;   // c24 = normal_y_flip, have_orme, pbr_strength, have_hd_albedo
constexpr UINT  EXTRA4_PS_REG = 25;   // c25 = vcol_mix, lm_mix, cc_override, albedo_boost

// Tunables (default values; hotkeys mutate at runtime)
struct Tunables {
    float bump_amp   = 5.0f;   // F3-/F4+
    float light_min  = 0.20f;  // F5/F6 narrows / widens
    float light_max  = 1.50f;
    float debug_mode = 0.0f;   // F2 cycles 0..3
    float sun_x      = 0.408f, sun_y = 0.408f, sun_z = 0.816f;  // pre-normalised
    void reset() { *this = Tunables{}; }
};
std::atomic<float> g_bump_amp{4.0f};
std::atomic<float> g_light_min{0.60f};   // v0.9: unused by Cook-Torrance path
std::atomic<float> g_light_max{1.00f};   // v0.9: REPURPOSED -> Exposure (g_params.z)
std::atomic<int>   g_debug_mode{0};

// Scene-reactive tunables — defaults conservative in v0.5.0
std::atomic<float> g_lm_driven   {0.0f};   // v0.5.0: fixed sun by default (lightmap-driven was producing wrong sun direction in some zones)
std::atomic<float> g_lm_amp      {4.0f};
std::atomic<float> g_spec_strength{0.15f}; // dialled back further
std::atomic<float> g_spec_shiny  {16.0f};
std::atomic<float> g_rim_strength{0.35f};  // v0.9: REPURPOSED -> Ambient (g_extra2.x)
std::atomic<float> g_lm_tap      {0.005f};

// Tracked screen size, refreshed each present.
std::atomic<float> g_screen_inv_w{1.0f / 1920.0f};
std::atomic<float> g_screen_inv_h{1.0f / 1080.0f};

// v0.4.8 — flip normal Y axis (DeepBump uses OpenGL Y-down convention,
// game expects Y-up). Default ON since users see inverted bumps.
std::atomic<float> g_normal_y_flip{1.0f};

// v0.6.1 — try real lightmap again now that we PreLoad() it per-draw.
// If the screenshot shows actual baked NC2 atmosphere, lm_mix stays 0.
// If still gray, default back to the v0.6.0 workaround.
std::atomic<float> g_vcol_mix    {0.0f};   // 0=use game vcol (works fine)
std::atomic<float> g_lm_mix      {0.0f};   // try the real lightmap again
std::atomic<float> g_cc_override {-1.0f};  // try game's cc again
std::atomic<float> g_albedo_boost{1.0f};
// v0.8: master strength for the ORME/PBR contribution (AO·rough·metal·emis)
std::atomic<float> g_pbr_strength{1.0f};

// Fallback sun direction (used when LM_DRIVEN < 1.0)
constexpr float SUN_X = 0.408f, SUN_Y = 0.408f, SUN_Z = 0.816f;

// m4 v0.4.8 — UNIFIED shader replacement (one HLSL handles all 5 known
// game PS variants — they all turned out to disassemble to byte-identical
// 7-instruction code: `albedo*vcol*lightmap*colorCorrection.x`).
//
// Critical bug fix from v0.4.7: vcol was being read from COLOR0; the
// game ACTUALLY uses TEXCOORD2 (verified via D3DDisassemble of the
// dumped 96f566cb.dxbc). That's why HUD elements rendered as solid
// gray rectangles — we were reading garbage from COLOR0.
//
// Other fixes also in this rev:
//   * Lightmap gradient sign was inverted (now +grad).
//   * Spec/rim modulated by lightmap so unlit areas don't glow.
//   * Alpha preservation: vanilla.a = albedo.a * vcol.a.
//   * Normal Y flip toggle (DeepBump uses Y-down).
//
// Constant register c4 = the GAME's colorCorrection (we read its value;
// the game writes it per draw). c20-c24 = our addon's tunables.
//
// Pixel-shader constants (c0..c7 reserved for game's world.ps):
//   c20.x = BUMP_AMP        (XY normal tilt, 0.5..15)
//   c20.y = LIGHT_MIN       (Lambertian floor)
//   c20.z = LIGHT_MAX       (Lambertian ceiling)
//   c20.w = DEBUG_MODE      (0=lit, 1=raw normal, 2=NdotL gray, 3=albedo)
//   c21.xyz = SUN_TS_FALLBACK   (used when LM_DRIVEN=0)
//   c21.w   = BUMP_ON       (1=per-tex active, 0=pass-through vanilla)
//   c22.x = LM_DRIVEN       (0=fixed sun, 1=lightmap-gradient sun)
//   c22.y = LM_GRAD_AMP     (gradient amplification, default 4)
//   c22.z = SPEC_STRENGTH   (specular blend, default 0.4)
//   c22.w = SPEC_SHININESS  (Phong exponent, default 16)
//   c23.x = RIM_STRENGTH    (fresnel rim, default 0.3)
//   c23.y = LM_TAP_OFFSET   (UV offset for gradient samples, default 0.005)
//   c23.zw  = SCREEN_SIZE   (1.0/width, 1.0/height — for VPOS view dir)
//
// Both world.ps and mesh.ps compile to ps_3_0 (DXVK supports trivially)
// so we get VPOS for screen-position-derived view direction.
//
// UNIFIED replacement — same HLSL handles world.ps + mesh.ps + variants.
// All four dumped game PS variants (mesh, 2 world-fog, overlay) had
// IDENTICAL disassembly: 7-instr `albedo*vcol*lightmap*colorCorrection.x`.
//
// Vanilla output (BUMP_ON=0):
//     rgb = albedo.rgb * vcol.rgb * lightmap.rgb * colorCorrection.x
//     a   = albedo.a   * vcol.a
//
// Bump path (BUMP_ON=1) adds normal-mapped diffuse + view-dependent
// spec / rim ON TOP of the vanilla colour.
const char* UNIFIED_PS_HLSL = R"hlsl(
sampler2D samplerAlbedo   : register(s0);   // v0.8: our HD regen replaces the game's clamped-512 albedo here
sampler2D samplerLightmap : register(s1);
sampler2D samplerNormal   : register(s2);   // v0.5.5: back to s2 (was s5 — sparse may have broken DXVK)
sampler2D samplerOrme     : register(s3);   // v0.8: R=occlusion G=roughness B=metallic A=emissive

float4 colorCorrection : register(c4);  // GAME's per-draw constant

float4 g_params : register(c20);
float4 g_sun    : register(c21);
float4 g_extra  : register(c22);
float4 g_extra2 : register(c23);
float4 g_extra3 : register(c24);
float4 g_extra4 : register(c25);   // v0.5.1 overrides + debug

float luminance(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 main(float2 uv     : TEXCOORD0,
            float2 uv_lm  : TEXCOORD1,
            float4 vcol_t : TEXCOORD2,       // skinned mesh / HUD uses this
            float4 vcol_c : COLOR0) : COLOR { // world.vs uses this (oD0)
    // ps_2_0: no VPOS available, use fixed forward view in tangent space
    float4 albedo   = tex2D(samplerAlbedo,   uv);
    float4 lightmap = tex2D(samplerLightmap, uv_lm);

    // The game's bytecode always reads from t2 — but on Wine/DXVK an
    // interpolant unwritten by the VS reads as (0,0,0,0) instead of
    // D3D9's reference default (1,1,1,1). World.vs writes oD0 (COLOR0),
    // mesh/HUD VS writes oT2. Pick whichever is non-zero, default to 1.
    float t2_active = saturate(dot(vcol_t, float4(1,1,1,1)));
    float c0_active = saturate(dot(vcol_c, float4(1,1,1,1)));
    float4 vcol = (t2_active > 0.001) ? vcol_t :
                  (c0_active > 0.001) ? vcol_c : float4(1,1,1,1);

    // v0.5.1 — override knobs (g_extra4) so the user can debug each
    // channel of the vanilla formula independently:
    //   c25.x = vcol_mix         0=use vcol, 1=force vcol=1
    //   c25.y = lm_mix           0=use lightmap, 1=force lm=1
    //   c25.z = cc_override      negative=use game cc, >=0 use this value
    //   c25.w = albedo_boost     scalar multiplier (default 1.0)
    float vcol_mix      = saturate(g_extra4.x);
    float lm_mix        = saturate(g_extra4.y);
    float cc_override   = g_extra4.z;
    float albedo_boost  = g_extra4.w;

    float3 vcol_eff = lerp(vcol.rgb, float3(1,1,1), vcol_mix);
    float  vcola_eff = lerp(vcol.a,  1.0,           vcol_mix);
    float3 lm       = lerp(lightmap.rgb, float3(1,1,1), lm_mix);
    float  cc       = (cc_override >= 0.0) ? cc_override : colorCorrection.x;
    float3 ab       = albedo.rgb * albedo_boost;

    // EXACT vanilla NC2 formula (with overridable factors)
    float3 vanilla_rgb = ab * vcol_eff * lm * cc;
    float  vanilla_a   = albedo.a * vcola_eff;

    // Per-channel debug viz BEFORE bump path so we can also inspect
    // non-per-tex draws. Mode 0 falls through to normal rendering.
    float dmode = g_params.w;
    if (dmode > 3.5 && dmode < 4.5) return float4(vanilla_rgb, 1.0);            // 4 = vanilla raw
    if (dmode > 4.5 && dmode < 5.5) return float4(albedo.rgb, 1.0);              // 5 = albedo channel
    if (dmode > 5.5 && dmode < 6.5) return float4(lightmap.rgb, 1.0);            // 6 = lightmap channel
    if (dmode > 6.5 && dmode < 7.5) return float4(vcol.rgb, 1.0);                // 7 = vcol channel
    if (dmode > 7.5 && dmode < 8.5) return float4(colorCorrection.xyz, 1.0);     // 8 = cc.xyz as RGB
    if (dmode > 8.5 && dmode < 9.5) return float4(vcol_eff, 1.0);                // 9 = vcol after mix
    if (dmode > 9.5 && dmode < 10.5) return float4(lm, 1.0);                     // 10 = lm after mix
    // v0.5.4 diagnostic: discriminate sampler-binding vs interpolant problems
    if (dmode > 10.5 && dmode < 11.5)
        return float4(tex2D(samplerLightmap, float2(0.5, 0.5)).rgb, 1.0);        // 11 = lm at fixed UV (0.5,0.5)
    if (dmode > 11.5 && dmode < 12.5)
        return float4(tex2D(samplerLightmap, uv).rgb, 1.0);                       // 12 = lm sampled with TEXCOORD0 instead of TEXCOORD1
    if (dmode > 12.5 && dmode < 13.5) {                                           // 13 = TEXCOORD1 raw (uv_lm as gradient)
        return float4(uv_lm, 0.0, 1.0);
    }
    if (dmode > 13.5 && dmode < 14.5)                                             // 14 = TEXCOORD0 raw
        return float4(uv, 0.0, 1.0);   // bounded: modes 15-19 fall through to ORME debug

    if (g_sun.w < 0.5) return float4(vanilla_rgb, vanilla_a);

    // mesh.ps (g_sun.w==2): minimal SAFE path — only sample our normal
    // at s2 (s0/s3 left untouched by the C++ side), no Cook-Torrance,
    // no extra lightmap fetch. Just a gentle normal-mapped relief on the
    // game's own vanilla mesh colour. Avoids the descriptor-isolation
    // crash that full PBR on the high-frequency mesh pipeline caused.
    if (g_sun.w > 1.5) {
        float3 nm = tex2D(samplerNormal, uv).rgb * 2.0 - 1.0;
        nm.y *= (g_extra3.x > 0.5) ? -1.0 : 1.0;
        float3 N2 = normalize(float3(nm.xy * g_params.x, nm.z));
        float bump = saturate(0.5 + 0.5 * N2.z);
        return float4(vanilla_rgb * lerp(0.82, 1.18, bump), vanilla_a);
    }

    // ───────────────────────── v0.9 Cook-Torrance ──────────────────────
    // The baked lightmap is treated as the INCIDENT RADIANCE (irradiance)
    // arriving at the surface — its intensity is the light, its gradient
    // the dominant direction. We light a real metallic-workflow GGX BRDF
    // with it instead of doing `albedo·lightmap` and smearing fake spec.
    // No scene/light list is available to a D3D9 PS, so the lightmap is
    // still the only world-lighting source — but used AS light, not as a
    // post-multiply. ps_2_b: tangent-space view (true screen VPOS needs
    // ps_3_0, deferred — would regress interpolant passthrough on DXVK).
    static const float PI = 3.14159265;

    float3 n_raw = tex2D(samplerNormal, uv).rgb * 2.0 - 1.0;
    n_raw.y *= (g_extra3.x > 0.5) ? -1.0 : 1.0;
    float3 N = normalize(float3(n_raw.xy * g_params.x, n_raw.z));

    float4 orme   = tex2D(samplerOrme, uv);
    float has_orme = saturate(g_extra3.y);
    float pbr_k    = g_extra3.z;
    float ao    = lerp(1.0, orme.r, has_orme);
    float rough = clamp(lerp(0.5, orme.g, has_orme), 0.045, 1.0);
    float metal = orme.b * has_orme;
    float emis  = orme.a * has_orme;

    // ORME per-channel debug (15-19)
    if (dmode > 14.5 && dmode < 15.5) return float4(ao.xxx, 1.0);
    if (dmode > 15.5 && dmode < 16.5) return float4(rough.xxx, 1.0);
    if (dmode > 16.5 && dmode < 17.5) return float4(metal.xxx, 1.0);
    if (dmode > 17.5 && dmode < 18.5) return float4(emis.xxx, 1.0);
    if (dmode > 18.5 && dmode < 19.5) return float4(orme.rgb, 1.0);

    // material (metallic workflow)
    float3 base_col = albedo.rgb * albedo_boost;
    float3 F0   = lerp(float3(0.04, 0.04, 0.04), base_col, metal);
    float3 diff_col = base_col * (1.0 - metal);

    // baked irradiance + its gradient-derived dominant direction
    float3 E   = lightmap.rgb;
    float lm_d = g_extra.x;                 // fixed-sun ↔ lightmap-grad
    float lm_a = g_extra.y;
    float d    = g_extra2.y;
    float L_c = luminance(E);
    float L_r = luminance(tex2D(samplerLightmap, uv_lm + float2(d, 0)).rgb);
    float L_u = luminance(tex2D(samplerLightmap, uv_lm + float2(0, d)).rgb);
    float3 sun_dyn = normalize(float3(float2(L_r - L_c, L_u - L_c) * lm_a, 1.0));
    float3 L = normalize(lerp(g_sun.xyz, sun_dyn, lm_d));
    float3 V = float3(0.0, 0.0, 1.0);       // tangent-space view (ps_2_b)
    float3 H = normalize(L + V);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V)) + 1e-4;
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // GGX specular: D (Trowbridge-Reitz), G (Schlick-GGX), F (Schlick)
    float a   = rough * rough;
    float a2  = a * a;
    float dnm = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D   = a2 / max(PI * dnm * dnm, 1e-5);
    float kg  = a * 0.5;
    float Gv  = NdotV / (NdotV * (1.0 - kg) + kg);
    float Gl  = NdotL / (NdotL * (1.0 - kg) + kg);
    float G   = Gv * Gl;
    float3 Fr = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float3 spec = (D * G) * Fr / max(4.0 * NdotV * NdotL, 1e-3);

    // ENERGY FIX (v0.9.1): the baked lightmap E is already-INTEGRATED
    // diffuse irradiance for the macro surface, NOT a punctual radiance.
    // So the base diffuse is albedo*E (≈ the vanilla albedo*lightmap
    // brightness — energy-preserving, not dark). The normal map only
    // adds a gentle DETAIL modulation around that — never a full Lambert
    // that crushes to black. Specular GGX is added on top, lit by E.
    float ndl = dot(N, L);
    float detail_mod = lerp(0.55, 1.35, saturate(0.5 + 0.5 * ndl));
    float3 diffuse  = diff_col * E * detail_mod;
    float3 specular = spec * E * saturate(ndl);      // spec = D*G*F already

    float amb_amt = g_extra2.x;                      // "Ambient" slider
    float3 color = (diffuse + specular) * ao
                 + diff_col * E * amb_amt * ao;      // IBL fill from lightmap
    color += base_col * emis * pbr_k * 2.0;          // self-illum neon
    color *= lerp(vcol_eff, float3(1,1,1), 0.5);     // mild vcol tint

    // exposure + ACES filmic tonemap
    float expo = max(g_params.z, 0.05);              // "Exposure" slider
    float gcc  = (cc_override >= 0.0) ? cc : 1.0;
    color *= expo * gcc;
    color = (color * (2.51 * color + 0.03)) /
            (color * (2.43 * color + 0.59) + 0.14);  // ACES approx

    float mode = g_params.w;
    if (mode > 0.5 && mode < 1.5) return float4(n_raw * 0.5 + 0.5, 1.0);
    if (mode > 1.5 && mode < 2.5) return float4(NdotL.xxx, 1.0);
    if (mode > 2.5)                return albedo;
    return float4(saturate(color), vanilla_a);
}
)hlsl";

// CRC list of game pixel shaders we substitute. They all happen to share
// the same 7-instruction body, so one replacement covers them all.
constexpr uint32_t MESH_PS_CRC    = 0x96f566cbu; // NPCs / items / weapons
constexpr uint32_t OVERLAY_PS_CRC = 0xeb1b9a91u; // cursor / damage flash

// v0.9.1: mesh.ps/overlay.ps substitution is OPT-IN via env var. History
// (v0.7) said substituting mesh.ps triggers DXVK descriptor isolation
// that grays the lightmap on world.ps too. The v0.9 architecture differs;
// this lets the in-game test decide WITHOUT regressing the working
// world path by default. Launch with NEOCRON_SUBSTITUTE_MESH=1 to A/B.
inline bool subst_extra() {
    static const bool v = [] {
        const char* e = std::getenv("NEOCRON_SUBSTITUTE_MESH");
        return e && *e && *e != '0';
    }();
    return v;
}
inline bool is_subst_target(uint32_t c) {
    for (uint32_t k : GAME_PS_CRCS) if (c == k) return true;
    // ONLY mesh.ps under the opt-in. overlay.ps (cursor/damage-flash)
    // is a 2D shader with a different I/O signature than our
    // Cook-Torrance replacement (no lightmap/TEXCOORD1) — substituting
    // it makes an invalid pipeline and crashes the D3D device (exit 5).
    if (subst_extra() && c == MESH_PS_CRC) return true;
    return false;
}

// ── CRC32 ────────────────────────────────────────────────────────────
uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & -(int32_t)(c & 1));
    }
    return c ^ 0xFFFFFFFFu;
}

// ── D3DCompile dynamic load ──────────────────────────────────────────
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

HMODULE        g_d3dc        = nullptr;
PFN_D3DCompile g_D3DCompile  = nullptr;
std::vector<uint8_t> g_unified_bytecode;
// v0.9.7: world.ps and mesh.ps each get their OWN replacement-bytecode
// buffer (identical content, distinct allocation/pointer). Sharing one
// `desc->code` pointer across two different game pipelines confused
// ReShade/DXVK pipeline-state keying → descriptor isolation crash when
// both coexist. Distinct pointers let DXVK isolate them.
std::vector<uint8_t> g_mesh_bytecode;
std::atomic<uint64_t> g_replacements_done{0};

bool compile_one(const char* hlsl, const char* tag, std::vector<uint8_t>& out) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errs = nullptr;
    // v0.5.3: ps_2_b instead of ps_2_0.
    // ps_2_0 has a 64-instruction limit; our unified shader (vanilla
    // formula + bump math + 11 debug-viz branches + scene-reactive
    // gradient) compiles to 138 instructions which doesn't fit.
    // ps_2_b is a profile extension of ps_2_0 with a 512-instruction
    // ceiling, same VS-PS interpolant routing and constant-table
    // semantics — so it doesn't reintroduce the v0.5.1 ps_3_0
    // game-state-passthrough bug. DXVK / Polaris support trivially.
    HRESULT hr = g_D3DCompile(hlsl, std::strlen(hlsl), tag,
        nullptr, nullptr, "main", "ps_2_b", 0, 0, &code, &errs);
    if (FAILED(hr) || !code) {
        char buf[400];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: D3DCompile FAILED for %s hr=0x%08x errs=%s",
            tag, (unsigned)hr,
            errs ? (const char*)errs->GetBufferPointer() : "(none)");
        reshade::log::message(reshade::log::level::error, buf);
        if (errs) errs->Release();
        return false;
    }
    out.assign(
        (const uint8_t*)code->GetBufferPointer(),
        (const uint8_t*)code->GetBufferPointer() + code->GetBufferSize());
    code->Release();
    if (errs) errs->Release();

    // Diagnostic: dump our compiled bytecode for offline disassembly.
    char dumppath[160];
    std::snprintf(dumppath, sizeof(dumppath), "Z:\\tmp\\our_%s.dxbc", tag);
    FILE* dump = std::fopen(dumppath, "wb");
    if (dump) { std::fwrite(out.data(), 1, out.size(), dump); std::fclose(dump); }

    char buf[200];
    std::snprintf(buf, sizeof(buf),
        "renodx-engine: replacement %s compiled, %zu bytes (dumped to %s)", tag, out.size(), dumppath);
    reshade::log::message(reshade::log::level::info, buf);
    return true;
}

bool compile_replacement() {
    g_d3dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!g_d3dc) {
        reshade::log::message(reshade::log::level::error,
            "renodx-engine: LoadLibrary d3dcompiler_47.dll failed");
        return false;
    }
    g_D3DCompile = (PFN_D3DCompile)GetProcAddress(g_d3dc, "D3DCompile");
    if (!g_D3DCompile) return false;
    if (!compile_one(UNIFIED_PS_HLSL, "unified", g_unified_bytecode))
        return false;
    // independent compile → independent DXBC object/pointer for mesh.ps
    compile_one(UNIFIED_PS_HLSL, "unified-mesh", g_mesh_bytecode);
    if (g_mesh_bytecode.empty()) g_mesh_bytecode = g_unified_bytecode;
    return true;
}

// ── default normal map (loaded once at first present) ────────────────
std::mutex g_normal_mu;
IDirect3DTexture9* g_default_normal = nullptr;
bool g_normal_load_attempted = false;

bool load_default_normal(IDirect3DDevice9* dev) {
    std::lock_guard<std::mutex> g(g_normal_mu);
    if (g_default_normal) return true;
    if (g_normal_load_attempted) return false;
    g_normal_load_attempted = true;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(DEFAULT_NORMAL_FILE, &w, &h, &channels, 4);
    if (!pixels) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: stbi_load(%s) failed: %s",
            DEFAULT_NORMAL_FILE, stbi_failure_reason());
        reshade::log::message(reshade::log::level::error, buf);
        return false;
    }

    HRESULT hr = dev->CreateTexture((UINT)w, (UINT)h, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_default_normal, nullptr);
    if (FAILED(hr) || !g_default_normal) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: CreateTexture FAILED hr=0x%08x for normal %dx%d",
            (unsigned)hr, w, h);
        reshade::log::message(reshade::log::level::error, buf);
        stbi_image_free(pixels);
        return false;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(g_default_normal->LockRect(0, &lr, nullptr, 0))) {
        reshade::log::message(reshade::log::level::error,
            "renodx-engine: LockRect failed for normal texture");
        g_default_normal->Release();
        g_default_normal = nullptr;
        stbi_image_free(pixels);
        return false;
    }
    // stb gives RGBA bytes; D3DFMT_A8R8G8B8 in memory is BGRA.
    // Swap R and B per pixel.
    for (int y = 0; y < h; ++y) {
        uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
        const unsigned char* src = pixels + y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x*4 + 0] = src[x*4 + 2];   // B
            dst[x*4 + 1] = src[x*4 + 1];   // G
            dst[x*4 + 2] = src[x*4 + 0];   // R
            dst[x*4 + 3] = src[x*4 + 3];   // A
        }
    }
    g_default_normal->UnlockRect(0);
    stbi_image_free(pixels);

    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "renodx-engine: default normal map loaded %dx%d, bound at sampler %u",
        w, h, NORMAL_SAMPLER);
    reshade::log::message(reshade::log::level::info, buf);
    return true;
}

// ── m4 per-texture normal lookup ─────────────────────────────────────
//
// Two layers of mapping:
//   index:  uint64_t hash      → relative path  (loaded once from .txt)
//   live:   uint64_t resource  → uint64_t hash  (built at init_resource)
//   cache:  uint64_t hash      → IDirect3DTexture9*  (lazy on first draw use)

std::mutex  g_index_mu;
// v0.8: each hash maps to a PBR triplet (albedo/normal/orme), full paths
// (corpus_root prepended at load time). Legacy 2-token lines (stock
// normal-only index) leave albedo/orme empty — handled gracefully.
struct PbrPaths { std::string albedo, normal, orme; };
std::unordered_map<uint64_t, PbrPaths> g_hash_to_path;

std::mutex  g_resource_mu;
std::unordered_map<uint64_t, uint64_t>    g_resource_to_hash;

// v0.8: cache the whole PBR triplet per hash. A hash with an index
// entry but a failed/absent normal is still a cached "no-op" (all null).
struct PbrTex { IDirect3DTexture9 *albedo, *normal, *orme; };
std::mutex  g_normal_cache_mu;
std::unordered_map<uint64_t, PbrTex> g_normal_cache;
// v0.9.6: textures evicted from the cache are NOT Released on the draw
// thread (they may still be bound / in GPU flight, and the draw thread
// must not stall on D3D destruction) — they're queued and freed at the
// frame boundary in on_present, when no draw references them.
std::mutex  g_pending_mu;
std::vector<IDirect3DTexture9*> g_pending_release;
inline void defer_release(IDirect3DTexture9* t) {
    if (!t) return;
    std::lock_guard<std::mutex> g(g_pending_mu);
    g_pending_release.push_back(t);
}
std::atomic<uint64_t> g_lazy_loaded{0};
std::atomic<uint64_t> g_index_hits{0};
std::atomic<uint64_t> g_index_misses{0};

inline uint64_t hash_pixels(const void* data, size_t avail, uint32_t w, uint32_t h) {
    if (!data || avail == 0) return 0;
    size_t take = avail < HASH_SAMPLE_BYTES ? avail : HASH_SAMPLE_BYTES;
    uint32_t crc = crc32(static_cast<const uint8_t*>(data), take);
    return ((uint64_t)crc << 32) | ((uint64_t)(w & 0xFFFFu) << 16) | (uint64_t)(h & 0xFFFFu);
}

// Load one index file and prepend the given corpus root to each entry's
// path. Returns (loaded, kept) — `kept` is the number of entries we
// actually inserted (collisions with already-loaded entries are skipped,
// preserving precedence from earlier calls).
std::pair<size_t, size_t> load_one_index(const char* path, const char* corpus_root) {
    FILE* f = std::fopen(path, "r");
    if (!f) return { 0, 0 };

    size_t loaded = 0, kept = 0;
    char line[1600];
    auto fix = [&](std::string s) {
        if (s.empty()) return s;
        std::string full = std::string(corpus_root) + s;
        for (auto& c : full) if (c == '/') c = '\\';
        return full;
    };
    while (std::fgets(line, sizeof(line), f)) {
        char hex[24] = {0}, a[512] = {0}, n[512] = {0}, o[512] = {0};
        int nf = std::sscanf(line, "%23s %511s %511s %511s", hex, a, n, o);
        if (nf < 2) continue;
        char* endp = nullptr;
        uint64_t h = std::strtoull(hex, &endp, 16);
        if (!h || endp == hex) continue;
        ++loaded;
        PbrPaths p;
        if (nf >= 4) {                       // v0.8 triplet line
            p.albedo = fix(a); p.normal = fix(n); p.orme = fix(o);
        } else {                              // legacy 2-token: normal only
            p.normal = fix(a);
        }
        // Insert only if hash isn't already present (preserves precedence).
        if (g_hash_to_path.emplace(h, std::move(p)).second) ++kept;
    }
    std::fclose(f);
    return { loaded, kept };
}

bool load_texture_index() {
    std::lock_guard<std::mutex> g(g_index_mu);
    g_hash_to_path.clear();

    // Override (dev machines etc): one env var, applied to whichever index
    // is loaded. If set, replaces both corpus roots; user is on their own
    // for stock vs HD layout.
    const char* env_override = std::getenv("NEOCRON_NORMALS_DIR");
    std::string hd_root    = env_override && *env_override ? env_override : HD_CORPUS_DIR;
    std::string stock_root = env_override && *env_override ? env_override : STOCK_CORPUS_DIR;
    auto ensure_sep = [](std::string& s) {
        if (!s.empty() && s.back() != '\\' && s.back() != '/') s.push_back('\\');
    };
    ensure_sep(hd_root);
    ensure_sep(stock_root);

    auto [hd_loaded, hd_kept]       = load_one_index(HD_INDEX_FILE,    hd_root.c_str());
    auto [stock_loaded, stock_kept] = load_one_index(STOCK_INDEX_FILE, stock_root.c_str());

    char buf[400];
    std::snprintf(buf, sizeof(buf),
        "renodx-engine: indexes loaded — HD %zu entries (root='%s'), "
        "stock %zu loaded / %zu kept (root='%s'); total map size = %zu",
        hd_kept, hd_root.c_str(),
        stock_loaded, stock_kept, stock_root.c_str(),
        g_hash_to_path.size());
    reshade::log::message(reshade::log::level::info, buf);

    if (g_hash_to_path.empty()) {
        reshade::log::message(reshade::log::level::warning,
            "renodx-engine: NO indexes found — per-texture lookup disabled, "
            "all surfaces will fall back to brick default normal");
        return false;
    }
    return true;
}

const PbrPaths* lookup_normal_path(uint64_t hash) {
    std::lock_guard<std::mutex> g(g_index_mu);
    auto it = g_hash_to_path.find(hash);
    return it != g_hash_to_path.end() ? &it->second : nullptr;
}

// PNG (RGBA) -> D3DFMT_A8R8G8B8 managed texture. nullptr on any failure
// or empty path. Caller owns the returned ref.
IDirect3DTexture9* load_png_d3d9(IDirect3DDevice9* dev,
                                 const std::string& full, uint64_t hash) {
    if (full.empty()) return nullptr;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(full.c_str(), &w, &h, &ch, 4);
    if (!px) {
        char buf[420];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: stbi_load FAILED %s (hash %016llx): %s",
            full.c_str(), (unsigned long long)hash, stbi_failure_reason());
        reshade::log::message(reshade::log::level::warning, buf);
        return nullptr;
    }
    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = dev->CreateTexture((UINT)w, (UINT)h, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    if (FAILED(hr) || !tex) { stbi_image_free(px); return nullptr; }
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
        tex->Release(); stbi_image_free(px); return nullptr;
    }
    for (int y = 0; y < h; ++y) {                  // RGBA -> BGRA
        uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
        const unsigned char* s = px + y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x*4+0] = s[x*4+2]; dst[x*4+1] = s[x*4+1];
            dst[x*4+2] = s[x*4+0]; dst[x*4+3] = s[x*4+3];
        }
    }
    tex->UnlockRect(0);
    stbi_image_free(px);
    return tex;
}

// Lazy-load the PBR triplet for a hash. Returns the PbrTex BY VALUE
// (v0.9.6): the previous `const PbrTex*` pointed INTO g_normal_cache —
// a concurrent insert here rehashes the unordered_map and invalidates
// every such pointer, so the draw thread used freed memory (textbook
// UAF; mesh.ps's draw frequency made it fire, +seh latency hid it).
// Returning a value copies the 3 raw texture pointers, which stay valid
// because eviction now DEFERS Release to the frame boundary.
PbrTex get_or_load_pbr(IDirect3DDevice9* dev, uint64_t hash) {
    {
        std::lock_guard<std::mutex> g(g_normal_cache_mu);
        auto it = g_normal_cache.find(hash);
        if (it != g_normal_cache.end()) return it->second;
    }
    const PbrPaths* rel = lookup_normal_path(hash);
    PbrTex t{ nullptr, nullptr, nullptr };
    if (rel) {
        t.albedo = load_png_d3d9(dev, rel->albedo, hash);
        t.normal = load_png_d3d9(dev, rel->normal, hash);
        t.orme   = load_png_d3d9(dev, rel->orme,   hash);
    }
    std::lock_guard<std::mutex> g(g_normal_cache_mu);
    auto it = g_normal_cache.find(hash);
    if (it != g_normal_cache.end()) {              // raced — drop ours
        defer_release(t.albedo); defer_release(t.normal); defer_release(t.orme);
        return it->second;
    }
    if (g_normal_cache.size() >= NORMAL_CACHE_MAX) {
        for (auto e = g_normal_cache.begin(); e != g_normal_cache.end(); ++e) {
            if (e->second.albedo || e->second.normal || e->second.orme) {
                defer_release(e->second.albedo);
                defer_release(e->second.normal);
                defer_release(e->second.orme);
                g_normal_cache.erase(e);
                break;
            }
        }
    }
    g_normal_cache[hash] = t;
    if (rel && (t.albedo || t.normal || t.orme)) {
        uint64_t n = g_lazy_loaded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n % 50) == 0) {
            char buf[420];
            std::snprintf(buf, sizeof(buf),
                "renodx-engine: lazy-loaded PBR #%llu hash=%016llx "
                "alb=%d n=%d orme=%d", (unsigned long long)n,
                (unsigned long long)hash, !!t.albedo, !!t.normal, !!t.orme);
            reshade::log::message(reshade::log::level::info, buf);
        }
    }
    return t;
}

// ── pipeline tracking ────────────────────────────────────────────────
std::mutex g_pipe_mu;
std::unordered_map<uint64_t, uint32_t> g_ps_pipeline_to_crc;
std::unordered_set<uint64_t>           g_substituted_ps_pipelines;
std::unordered_set<uint32_t>           g_dumped_ps;

thread_local bool     t_z_enabled = true;
thread_local bool     t_using_substituted_world_ps = false;
thread_local uint32_t t_cur_ps_crc = 0;   // original CRC of bound (substituted) PS

// ── ReShade callbacks ────────────────────────────────────────────────

bool on_create_pipeline(reshade::api::device*,
                        reshade::api::pipeline_layout,
                        uint32_t subobject_count,
                        const reshade::api::pipeline_subobject* subobjects) {
    if (g_unified_bytecode.empty()) return false;
    bool modified = false;
    for (uint32_t i = 0; i < subobject_count; ++i) {
        const auto& s = subobjects[i];
        if (s.type != reshade::api::pipeline_subobject_type::pixel_shader) continue;
        auto* desc = static_cast<reshade::api::shader_desc*>(s.data);
        if (!desc || !desc->code || !desc->code_size) continue;
        uint32_t c = crc32(static_cast<const uint8_t*>(desc->code), desc->code_size);

        if (!is_subst_target(c)) continue;

        // distinct buffer per game pipeline (v0.9.7)
        auto& bc = (c == MESH_PS_CRC) ? g_mesh_bytecode : g_unified_bytecode;
        desc->code      = bc.data();
        desc->code_size = bc.size();
        modified = true;
        uint64_t n = g_replacements_done.fetch_add(1, std::memory_order_relaxed) + 1;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: game PS %08x create_pipeline #%llu — substituted (%zu bytes)",
            (unsigned)c, (unsigned long long)n, g_unified_bytecode.size());
        reshade::log::message(reshade::log::level::info, buf);
    }
    return modified;
}

void dump_ps_bytecode(uint32_t crc, const void* code, size_t size) {
    char path[160];
    std::snprintf(path, sizeof(path), "Z:/tmp/neocron_ps_%08x.dxbc", crc);
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fwrite(code, 1, size, f); std::fclose(f); }
}

void on_init_pipeline(reshade::api::device*,
                      reshade::api::pipeline_layout,
                      uint32_t subobject_count,
                      const reshade::api::pipeline_subobject* subobjects,
                      reshade::api::pipeline pipeline) {
    for (uint32_t i = 0; i < subobject_count; ++i) {
        const auto& s = subobjects[i];
        if (s.type != reshade::api::pipeline_subobject_type::pixel_shader) continue;
        auto* desc = static_cast<reshade::api::shader_desc*>(s.data);
        if (!desc || !desc->code || !desc->code_size) continue;
        uint32_t c = crc32(static_cast<const uint8_t*>(desc->code), desc->code_size);
        std::lock_guard<std::mutex> g(g_pipe_mu);
        // desc->code here is ALREADY our replacement (create_pipeline ran
        // first). Identify world vs mesh by which distinct buffer backs
        // it — this is the reliable mesh tag the crc could never give
        // (both replacements share content → same crc).
        bool is_world_repl = (desc->code == g_unified_bytecode.data());
        bool is_mesh_repl  = (!g_mesh_bytecode.empty() &&
                              desc->code == g_mesh_bytecode.data());
        if (is_mesh_repl) {
            g_ps_pipeline_to_crc[pipeline.handle] = MESH_PS_CRC;
            g_substituted_ps_pipelines.insert(pipeline.handle);
        } else if (is_world_repl) {
            g_ps_pipeline_to_crc[pipeline.handle] = WORLD_PS_CRC;
            g_substituted_ps_pipelines.insert(pipeline.handle);
        } else {
            g_ps_pipeline_to_crc[pipeline.handle] = c;
        }
        bool is_known = is_subst_target(c) || is_world_repl || is_mesh_repl;
        if (g_dumped_ps.insert(c).second && !is_known)
            dump_ps_bytecode(c, desc->code, desc->code_size);
    }
}

void on_bind_pipeline(reshade::api::command_list*,
                      reshade::api::pipeline_stage stages,
                      reshade::api::pipeline pipeline) {
    if (!((uint32_t)stages & (uint32_t)reshade::api::pipeline_stage::pixel_shader))
        return;
    std::lock_guard<std::mutex> g(g_pipe_mu);
    t_using_substituted_world_ps = g_substituted_ps_pipelines.count(pipeline.handle) > 0;
    auto it = g_ps_pipeline_to_crc.find(pipeline.handle);
    t_cur_ps_crc = (it != g_ps_pipeline_to_crc.end()) ? it->second : 0;
}

void on_bind_pipeline_states(reshade::api::command_list*,
                             uint32_t count,
                             const reshade::api::dynamic_state* states,
                             const uint32_t* values) {
    for (uint32_t i = 0; i < count; ++i)
        if (states[i] == reshade::api::dynamic_state::depth_enable)
            t_z_enabled = (values[i] != 0);
}

// ── m4 resource hashing ─────────────────────────────────────────────
//
// We tried map/unmap_texture_region first — D3D9's CreateTexture has
// no initial_data, so we used the LockRect/UnlockRect events to hash
// the upload. That crashed: DXVK appears to release the staging
// buffer mapping by the time ReShade fires unmap_texture_region, so
// reading the captured pointer hits unmapped memory → SEGV.
//
// Current approach: hash on first bind. When bind_normal_for_draw
// sees a texture at sampler 0 we haven't seen before, QueryInterface
// to IDirect3DTexture9, LockRect(D3DLOCK_READONLY) to get the system-
// memory copy (POOL_MANAGED has one; POOL_DEFAULT may not, in which
// case LockRect fails and we cache hash=0 = "miss"). Each game
// texture is hashed exactly once over its lifetime.
//
// init_resource is still wired so we can short-circuit on dims, but
// it's no longer the primary hash trigger.

struct ResourceDims { uint32_t w, h; reshade::api::resource_type type; };

std::mutex g_resource_dims_mu;
std::unordered_map<uint64_t, ResourceDims> g_resource_dims;

void on_init_resource(reshade::api::device*,
                      const reshade::api::resource_desc& desc,
                      const reshade::api::subresource_data* initial_data,
                      reshade::api::resource_usage,
                      reshade::api::resource resource) {
    if (desc.type != reshade::api::resource_type::texture_2d) return;

    {
        std::lock_guard<std::mutex> g(g_resource_dims_mu);
        g_resource_dims[resource.handle] =
            { desc.texture.width, desc.texture.height, desc.type };
    }

    // Rare path — if D3D9 ever passes initial_data (non-D3D9 backends do),
    // hash directly here.
    if (!initial_data || !initial_data->data) return;
    size_t avail = initial_data->slice_pitch
                     ? (size_t)initial_data->slice_pitch
                     : (size_t)initial_data->row_pitch * desc.texture.height;
    if (avail == 0) return;
    uint64_t hash = hash_pixels(initial_data->data, avail,
                                desc.texture.width, desc.texture.height);
    if (!hash) return;
    {
        std::lock_guard<std::mutex> g(g_resource_mu);
        g_resource_to_hash[resource.handle] = hash;
    }
    if (lookup_normal_path(hash))
        g_index_hits.fetch_add(1, std::memory_order_relaxed);
    else
        g_index_misses.fetch_add(1, std::memory_order_relaxed);
}

// Hash a D3D9 texture by locking its level-0 system-memory copy.
// Returns 0 on any failure (LockRect failed, dims invalid, etc.).
// Caller is responsible for caching the result so we only do this
// once per resource lifetime.
uint64_t hash_d3d9_texture(IDirect3DBaseTexture9* base) {
    if (!base) return 0;

    IDirect3DTexture9* tex2d = nullptr;
    if (FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&tex2d)) || !tex2d)
        return 0;

    uint64_t hash = 0;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(tex2d->GetLevelDesc(0, &desc))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex2d->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
            size_t avail = (size_t)lr.Pitch * desc.Height;
            if (lr.pBits && avail > 0)
                hash = hash_pixels(lr.pBits, avail, desc.Width, desc.Height);
            tex2d->UnlockRect(0);
        }
    }
    tex2d->Release();
    return hash;
}

void on_destroy_resource(reshade::api::device*, reshade::api::resource resource) {
    {
        std::lock_guard<std::mutex> g(g_resource_mu);
        g_resource_to_hash.erase(resource.handle);
    }
    {
        std::lock_guard<std::mutex> g(g_resource_dims_mu);
        g_resource_dims.erase(resource.handle);
    }
    // Normal cache is keyed on hash, not resource — leave it intact.
}

std::atomic<uint64_t> g_world_draws{0};
std::atomic<uint64_t> g_world_draws_per_tex_hit{0};

// v0.5.6 diagnostic: at each world draw, query sampler 1 / 2 / 5 to confirm
// game's bind state. If lightmap (s1) is consistently NULL, our shader
// can't possibly read it — we'd need a different binding strategy.
std::atomic<uint64_t> g_lm_bound_cnt{0};
std::atomic<uint64_t> g_lm_null_cnt{0};

inline void bind_normal_for_draw(reshade::api::command_list* cmd) {
    if (!t_using_substituted_world_ps) return;   // not our shader, don't touch
    auto* dev = reinterpret_cast<IDirect3DDevice9*>(cmd->get_device()->get_native());
    if (!dev) return;

    // Per-tex lookup only for 3D world geometry (z-test on). HUD draws
    // (z-test off) skip the lookup AND the normal binding, but we STILL
    // push BUMP_ON=0 constants so the shader takes the vanilla path.
    PbrTex pbr{ nullptr, nullptr, nullptr };   // v0.9.6: by value (no map ptr)
    if (t_z_enabled) {
        IDirect3DBaseTexture9* albedo = nullptr;
        dev->GetTexture(0, &albedo);
        if (albedo) {
            uint64_t handle = (uint64_t)albedo;
            uint64_t hash   = 0;
            bool need_hash  = false;
            {
                std::lock_guard<std::mutex> g(g_resource_mu);
                auto it = g_resource_to_hash.find(handle);
                if (it != g_resource_to_hash.end()) hash = it->second;
                else need_hash = true;
            }
            if (need_hash) {
                hash = hash_d3d9_texture(albedo);
                {
                    std::lock_guard<std::mutex> g(g_resource_mu);
                    g_resource_to_hash[handle] = hash;
                }
                if (hash) {
                    if (lookup_normal_path(hash))
                        g_index_hits.fetch_add(1, std::memory_order_relaxed);
                    else
                        g_index_misses.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (hash) pbr = get_or_load_pbr(dev, hash);
            albedo->Release();
        }
    }

    // per_tex = at least the normal resolved. albedo (s0) and orme (s3)
    // are bound when present; the shader senses each via g_extra2.zw.
    bool per_tex = pbr.normal != nullptr;
    auto bind = [&](UINT samp, IDirect3DTexture9* t) {
        dev->SetTexture(samp, t);
        dev->SetSamplerState(samp, D3DSAMP_ADDRESSU,  D3DTADDRESS_WRAP);
        dev->SetSamplerState(samp, D3DSAMP_ADDRESSV,  D3DTADDRESS_WRAP);
        dev->SetSamplerState(samp, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(samp, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(samp, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    };
    // Option A: mesh.ps is an ultra-high-frequency pipeline. Rebinding
    // the game's ACTIVE s0 (and s3) on it + DXVK descriptor isolation
    // hard-crashes (v0.9.x test). For mesh.ps draws, ONLY bind the
    // normal at s2 (a sampler the game never uses — the v0.7-safe slot);
    // never touch s0/s3 and never let the shader sample the lightmap.
    bool is_mesh = (t_cur_ps_crc == MESH_PS_CRC);
    bool have_hd_albedo = false, have_orme = false;
    if (per_tex) {
        bind(NORMAL_SAMPLER, pbr.normal);
        if (!is_mesh) {
            if (pbr.albedo) { bind(ALBEDO_SAMPLER, pbr.albedo); have_hd_albedo = true; }
            if (pbr.orme)   { bind(ORME_SAMPLER,   pbr.orme);   have_orme = true; }
        }
    }

    // v0.5.9: force MAXMIPLEVEL=0 + no mip selection on s1.
    // The dumped lightmap (PNG inspection) shows real bright neon
    // patterns on dark background, but sampling returns uniform
    // gray ≈ the texture's average colour — classic symptom of
    // GPU picking the smallest mip (1×1 = whole-texture average)
    // because derivatives aren't being computed correctly under
    // our substituted PS pipeline.
    if (t_z_enabled) {
        dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(1, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(1, D3DSAMP_MIPMAPLODBIAS, 0);
        dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        // Also for sampler 0 (albedo) and 2 (normal) — same rationale
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(2, D3DSAMP_MAXMIPLEVEL, 0);
    }

    // v0.6.1 — PreLoad() workaround: force DXVK to re-upload lightmap
    // VRAM before each sample. Theory: DXVK only uploads texture content
    // when the ORIGINAL pipeline references it; our substituted pipeline
    // doesn't trigger the upload, so VRAM stays uniform-grey placeholder.
    // PreLoad() is a no-op for GPU-already-uploaded textures, cheap.
    if (t_z_enabled) {
        IDirect3DBaseTexture9* lm_check = nullptr;
        dev->GetTexture(1, &lm_check);
        if (lm_check) {
            g_lm_bound_cnt.fetch_add(1, std::memory_order_relaxed);
            lm_check->PreLoad();
            lm_check->Release();
        } else {
            g_lm_null_cnt.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ALWAYS push constants — even for HUD draws, so BUMP_ON=0 is
    // explicit and the shader doesn't read stale BUMP_ON=1 from a
    // previous world.ps draw (which was the v0.4.7 UI-corruption bug).
    float params[4] = {
        g_bump_amp.load(std::memory_order_relaxed),
        g_light_min.load(std::memory_order_relaxed),
        g_light_max.load(std::memory_order_relaxed),
        (float)g_debug_mode.load(std::memory_order_relaxed)
    };
    // g_sun.w: 0=not per-tex (vanilla), 1=per-tex world (full PBR),
    //          2=per-tex mesh (normal-bump only, no lightmap/Cook-Torrance)
    float sun_w = !per_tex ? 0.0f : (is_mesh ? 2.0f : 1.0f);
    float sun[4]    = { SUN_X, SUN_Y, SUN_Z, sun_w };
    float extra[4]  = {
        g_lm_driven.load(),  g_lm_amp.load(),
        g_spec_strength.load(), g_spec_shiny.load()
    };
    float extra2[4] = {
        g_rim_strength.load(), g_lm_tap.load(),
        g_screen_inv_w.load(), g_screen_inv_h.load()
    };
    float extra3[4] = { g_normal_y_flip.load(),
                        have_orme ? 1.0f : 0.0f,
                        g_pbr_strength.load(),
                        have_hd_albedo ? 1.0f : 0.0f };
    float extra4[4] = {
        g_vcol_mix.load(),   g_lm_mix.load(),
        g_cc_override.load(), g_albedo_boost.load()
    };
    dev->SetPixelShaderConstantF(PARAMS_PS_REG, params, 1);
    dev->SetPixelShaderConstantF(SUN_PS_REG,    sun,    1);
    dev->SetPixelShaderConstantF(EXTRA_PS_REG,  extra,  1);
    dev->SetPixelShaderConstantF(EXTRA2_PS_REG, extra2, 1);
    dev->SetPixelShaderConstantF(EXTRA3_PS_REG, extra3, 1);
    dev->SetPixelShaderConstantF(EXTRA4_PS_REG, extra4, 1);

    g_world_draws.fetch_add(1, std::memory_order_relaxed);
    if (per_tex) {
        uint64_t n = g_world_draws_per_tex_hit.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "renodx-engine: FIRST per-texture PBR bound "
                "(n=%p alb=%p orme=%p)",
                (void*)pbr.normal, (void*)pbr.albedo, (void*)pbr.orme);
            reshade::log::message(reshade::log::level::info, buf);
        }
    }
}

bool on_draw(reshade::api::command_list* cmd, uint32_t, uint32_t, uint32_t, uint32_t) {
    bind_normal_for_draw(cmd);
    return false;
}
bool on_draw_indexed(reshade::api::command_list* cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
    bind_normal_for_draw(cmd);
    return false;
}

std::atomic<uint64_t> g_frame{0};

void log_tunables(const char* trigger) {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
        "renodx-engine: tunables [%s] amp=%.1f light=[%.2f..%.2f] debug=%d",
        trigger,
        g_bump_amp.load(), g_light_min.load(), g_light_max.load(),
        g_debug_mode.load());
    reshade::log::message(reshade::log::level::info, buf);
}

// ImGui overlay tab — Home → Add-ons → Neocron RenoDX Engine.
void on_overlay(reshade::api::effect_runtime*) {
    float amp   = g_bump_amp.load();
    float lmin  = g_light_min.load();
    float lmax  = g_light_max.load();
    int   mode  = g_debug_mode.load();

    ImGui::TextUnformatted("Per-texture normal mapping");
    ImGui::Separator();

    if (ImGui::SliderFloat("Bump amp",  &amp,  0.5f, 15.0f, "%.1f"))  g_bump_amp.store(amp);
    if (ImGui::SliderFloat("Exposure",  &lmax, 0.10f, 4.0f, "%.2f"))  g_light_max.store(lmax);   // v0.9 g_params.z
    if (ImGui::SliderFloat("(unused) light_min", &lmin, 0.0f, 1.0f, "%.2f")) g_light_min.store(lmin);

    // ASCII only — ImGui's default font has no em-dash glyph (renders '?')
    static const char* MODES[] = {
        "0 - Lit (full Cook-Torrance)",
        "1 - Raw normal RGB",
        "2 - NdotL grayscale",
        "3 - Albedo (per-tex only)",
        "4 - Vanilla raw (no math)",
        "5 - Albedo channel (HD if matched)",
        "6 - Lightmap (= irradiance)",
        "7 - vcol channel",
        "8 - colorCorrection.xyz",
        "9 - vcol after mix slider",
        "10 - Lightmap after mix slider",
        "11 - Lightmap @ fixed UV(0.5,0.5)",
        "12 - Lightmap sampled w/ TEXCOORD0",
        "13 - TEXCOORD1 raw (uv_lm gradient)",
        "14 - TEXCOORD0 raw (uv gradient)",
        "15 - ORME R: Occlusion",
        "16 - ORME G: Roughness",
        "17 - ORME B: Metallic",
        "18 - ORME A: Emissive",
        "19 - ORME RGB (O,R,M)",
    };
    if (ImGui::Combo("Debug viz", &mode, MODES, IM_ARRAYSIZE(MODES))) g_debug_mode.store(mode);

    ImGui::Separator();
    ImGui::TextUnformatted("Scene-reactive lighting");
    float lmd = g_lm_driven.load();
    float lma = g_lm_amp.load();
    float lmt = g_lm_tap.load();
    if (ImGui::SliderFloat("Lightmap-driven (vs fixed sun)", &lmd, 0.0f, 1.0f, "%.2f")) g_lm_driven.store(lmd);
    if (ImGui::SliderFloat("Gradient amp",                   &lma, 0.0f, 20.0f, "%.1f")) g_lm_amp.store(lma);
    if (ImGui::SliderFloat("Lightmap tap offset",            &lmt, 0.001f, 0.02f, "%.4f")) g_lm_tap.store(lmt);

    ImGui::Separator();
    ImGui::TextUnformatted("View-dependent specular");
    float ss = g_spec_strength.load();
    float sh = g_spec_shiny.load();
    float rs = g_rim_strength.load();
    if (ImGui::SliderFloat("Ambient (IBL from lightmap)", &rs, 0.0f, 1.5f, "%.2f")) g_rim_strength.store(rs);  // v0.9 g_extra2.x
    ImGui::TextDisabled("(legacy spec sliders inert under Cook-Torrance)");
    (void)ss; (void)sh;

    ImGui::Separator();
    ImGui::TextUnformatted("PBR (ORME: occlusion / roughness / metallic / emissive)");
    float pbrs = g_pbr_strength.load();
    if (ImGui::SliderFloat("PBR strength", &pbrs, 0.0f, 2.0f, "%.2f")) g_pbr_strength.store(pbrs);

    ImGui::Separator();
    ImGui::TextUnformatted("Vanilla formula overrides (v0.5.1 debug)");
    float vmix = g_vcol_mix.load();
    float lmix = g_lm_mix.load();
    float ccov = g_cc_override.load();
    float aboost = g_albedo_boost.load();
    if (ImGui::SliderFloat("vcol -> 1.0 mix",        &vmix, 0.0f, 1.0f, "%.2f")) g_vcol_mix.store(vmix);
    if (ImGui::SliderFloat("lightmap -> 1.0 mix",    &lmix, 0.0f, 1.0f, "%.2f")) g_lm_mix.store(lmix);
    if (ImGui::SliderFloat("colorCorrection override (-1=game)", &ccov, -1.0f, 2.0f, "%.2f")) g_cc_override.store(ccov);
    if (ImGui::SliderFloat("albedo boost",           &aboost, 0.5f, 4.0f, "%.2f")) g_albedo_boost.store(aboost);

    ImGui::Separator();
    ImGui::TextUnformatted("Normal map convention");
    bool y_flip = g_normal_y_flip.load() > 0.5f;
    if (ImGui::Checkbox("Flip normal Y (DeepBump=ON, Y-up=OFF)", &y_flip))
        g_normal_y_flip.store(y_flip ? 1.0f : 0.0f);

    if (ImGui::Button("Reset to defaults")) {
        g_bump_amp.store(4.0f);
        g_light_min.store(0.60f);
        g_light_max.store(1.00f);    // v0.9 Exposure
        g_debug_mode.store(0);
        g_lm_driven.store(0.0f);
        g_lm_amp.store(4.0f);
        g_lm_tap.store(0.005f);
        g_spec_strength.store(0.15f);
        g_spec_shiny.store(16.0f);
        g_rim_strength.store(0.35f); // v0.9 Ambient
        g_normal_y_flip.store(1.0f);
        g_vcol_mix.store(0.0f);
        g_lm_mix.store(0.0f);
        g_cc_override.store(-1.0f);
        g_albedo_boost.store(1.0f);
        log_tunables("ImGui reset");
    }

    ImGui::Separator();
    ImGui::Text("Stats");
    ImGui::Text("  world draws : %llu", (unsigned long long)g_world_draws.load());
    ImGui::Text("  per-tex hits: %llu", (unsigned long long)g_world_draws_per_tex_hit.load());
    ImGui::Text("  index hits  : %llu  misses: %llu",
                (unsigned long long)g_index_hits.load(),
                (unsigned long long)g_index_misses.load());
    ImGui::Text("  lazy-loaded : %llu  cache: %zu",
                (unsigned long long)g_lazy_loaded.load(),
                g_normal_cache.size());
    ImGui::Text("  index map   : %zu entries", g_hash_to_path.size());
    ImGui::Text("  s1 bound: %llu  s1 NULL: %llu  (ratio %.1f%%)",
                (unsigned long long)g_lm_bound_cnt.load(),
                (unsigned long long)g_lm_null_cnt.load(),
                100.0 * g_lm_bound_cnt.load() /
                    std::max<uint64_t>(1, g_lm_bound_cnt.load() + g_lm_null_cnt.load()));
}

void on_present(reshade::api::effect_runtime* rt) {
    // v0.9.6: free cache-evicted textures HERE — frame boundary, no draw
    // in flight, so nothing the GPU/device still references is destroyed.
    {
        std::vector<IDirect3DTexture9*> drain;
        {
            std::lock_guard<std::mutex> g(g_pending_mu);
            drain.swap(g_pending_release);
        }
        for (auto* t : drain) if (t) t->Release();
    }
    // Lazy-load normal on the first frame after the device is fully up.
    if (!g_default_normal && rt) {
        auto* dev = reinterpret_cast<IDirect3DDevice9*>(rt->get_device()->get_native());
        if (dev) load_default_normal(dev);
    }
    // Refresh screen size for VPOS-based view-direction calc. Cheap; only
    // changes on resize but covers windowed-mode resizes for free.
    if (rt) {
        auto* dev = reinterpret_cast<IDirect3DDevice9*>(rt->get_device()->get_native());
        if (dev) {
            D3DVIEWPORT9 vp{};
            if (SUCCEEDED(dev->GetViewport(&vp)) && vp.Width && vp.Height) {
                g_screen_inv_w.store(1.0f / (float)vp.Width,  std::memory_order_relaxed);
                g_screen_inv_h.store(1.0f / (float)vp.Height, std::memory_order_relaxed);
            }
        }
    }
    uint64_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((f % 1800) == 0) {
        char buf[400];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: frames=%llu replacements=%llu world-draws=%llu(per-tex %llu) "
            "index-hits=%llu/misses=%llu lazy-loaded=%llu cache-size=%zu default-normal=%d",
            (unsigned long long)f,
            (unsigned long long)g_replacements_done.load(),
            (unsigned long long)g_world_draws.load(),
            (unsigned long long)g_world_draws_per_tex_hit.load(),
            (unsigned long long)g_index_hits.load(),
            (unsigned long long)g_index_misses.load(),
            (unsigned long long)g_lazy_loaded.load(),
            g_normal_cache.size(),
            g_default_normal ? 1 : 0);
        reshade::log::message(reshade::log::level::info, buf);
    }
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "neocron-renodx-engine";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Tier 4 m4 — per-texture normal-mapped Phong on world.ps. "
    "Hashes every game texture at creation, looks up matching AI-generated "
    "normal map from the offline corpus, binds at sampler 5.";

BOOL WINAPI DllMain(HINSTANCE hmod, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(hmod)) return FALSE;
            compile_replacement();
            load_texture_index();
            reshade::register_event<reshade::addon_event::create_pipeline>(on_create_pipeline);
            reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
            reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
            reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
            reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
            reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
            reshade::register_event<reshade::addon_event::draw>(on_draw);
            reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
            reshade::register_event<reshade::addon_event::reshade_present>(on_present);
            reshade::register_overlay("Neocron RenoDX Engine", on_overlay);
            {
                char b[200];
                std::snprintf(b, sizeof(b),
                    "renodx-engine: v0.9.7 registered — world.ps always; "
                    "mesh.ps substitution=%s (NEOCRON_SUBSTITUTE_MESH)",
                    subst_extra() ? "ON" : "OFF");
                reshade::log::message(reshade::log::level::info, b);
            }
            log_tunables("init defaults");
            reshade::log::message(reshade::log::level::info,
                "renodx-engine: open ReShade overlay (Home) → Add-ons tab → "
                "'Neocron RenoDX Engine' for sliders + debug viz");
            break;
        case DLL_PROCESS_DETACH:
            reshade::unregister_overlay("Neocron RenoDX Engine", on_overlay);
            reshade::unregister_event<reshade::addon_event::reshade_present>(on_present);
            reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
            reshade::unregister_event<reshade::addon_event::draw>(on_draw);
            reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
            reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource);
            reshade::unregister_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
            reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
            reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
            reshade::unregister_event<reshade::addon_event::create_pipeline>(on_create_pipeline);
            reshade::unregister_addon(hmod);
            if (g_default_normal) { g_default_normal->Release(); g_default_normal = nullptr; }
            {
                std::lock_guard<std::mutex> g(g_normal_cache_mu);
                for (auto& kv : g_normal_cache) {
                    if (kv.second.albedo) kv.second.albedo->Release();
                    if (kv.second.normal) kv.second.normal->Release();
                    if (kv.second.orme)   kv.second.orme->Release();
                }
                g_normal_cache.clear();
            }
            if (g_d3dc) FreeLibrary(g_d3dc);
            break;
    }
    return TRUE;
}
