// neocron-renodx-engine — Tier 4 milestone 2: world.ps replacement (red tint test).
//
// Milestone 1 identified world.ps as PS CRC 0x2ccf5eb7 (496-byte ps_2_0,
// constants: samplerAlbedo / samplerLightmap / colorCorrection). This
// milestone hooks create_pipeline to SUBSTITUTE that shader at creation
// time with our own HLSL-compiled replacement. The first replacement is
// deliberately stupid: red channel × 1.5 — visually unmistakable, lets us
// verify the hook works before tackling normal mapping.
//
// Pipeline:
//   1. DllMain: load d3dcompiler_47.dll, look up D3DCompile.
//   2. Compile WORLD_PS_HLSL once into a ps_2_0 bytecode blob held in g_*.
//   3. Hook addon_event::create_pipeline. CRC the incoming PS. If it
//      matches 0x2ccf5eb7, point its desc->code/.code_size at our blob.
//   4. Watch the world for a red tint — if so, we're in.
//
// Also keeps the milestone-1 histogram for diagnostic continuity.

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace {

// ── tunables ─────────────────────────────────────────────────────────
constexpr uint32_t WORLD_PS_CRC = 0x2ccf5eb7u;

// First replacement is intentionally "loud" so we can confirm the hook
// works visually. Iterate this string in subsequent milestones to add
// real normal-mapped lighting.
// v0.2 — luminance-bump fake normal mapping. Derives a pseudo-tangent
// normal from the albedo's luminance gradient (small offsets in U/V),
// dots it against a hardcoded sun direction, modulates the
// vanilla-equivalent albedo*lightmap*2 base. No external normal-map
// texture needed — every wall/floor surface gets bump-style highlights
// in proportion to its texture's high-contrast detail (panel seams,
// brick grout, carpet weave). ps_2_0-friendly (~14 instructions).
//
// Per-texture AI-generated normal-map sampling is m3 — needs SetTexture
// interception + sibling lookup against the _normal_pipeline/ corpus,
// real engineering work for the next session.
const char* WORLD_PS_HLSL = R"hlsl(
sampler2D samplerAlbedo   : register(s0);
sampler2D samplerLightmap : register(s1);

static const float3 LUMA = float3(0.299, 0.587, 0.114);
static const float3 SUN_TS = float3(0.408, 0.408, 0.816);   // pre-normalised
static const float  STEP = 0.005;     // UV offset for gradient (~half a 256² texel)
static const float  BUMP = 3.0;       // gradient → normal slope multiplier

float4 main(float2 uv : TEXCOORD0, float2 uv_lm : TEXCOORD1, float4 vcol : COLOR0) : COLOR {
    float4 albedo   = tex2D(samplerAlbedo,   uv);
    float4 lightmap = tex2D(samplerLightmap, uv_lm);

    // Pseudo-normal from albedo luminance gradient
    float lc = dot(albedo.rgb,                                     LUMA);
    float lr = dot(tex2D(samplerAlbedo, uv + float2(STEP, 0)).rgb, LUMA);
    float lu = dot(tex2D(samplerAlbedo, uv + float2(0, STEP)).rgb, LUMA);
    float3 n  = normalize(float3((lc - lr) * BUMP, (lc - lu) * BUMP, 1.0));

    // Lambertian against fixed sun, lifted by ambient floor
    float NdotL = saturate(dot(n, SUN_TS));
    float light = 0.65 + 0.35 * NdotL;

    return saturate(albedo * lightmap * 2.0 * light);
}
)hlsl";

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

HMODULE       g_d3dc        = nullptr;
PFN_D3DCompile g_D3DCompile = nullptr;
std::vector<uint8_t> g_replacement_bytecode;
std::atomic<uint64_t> g_replacements_done{0};

bool compile_replacement() {
    g_d3dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!g_d3dc) {
        reshade::log::message(reshade::log::level::error,
            "renodx-engine: LoadLibrary d3dcompiler_47.dll failed");
        return false;
    }
    g_D3DCompile = (PFN_D3DCompile)GetProcAddress(g_d3dc, "D3DCompile");
    if (!g_D3DCompile) {
        reshade::log::message(reshade::log::level::error,
            "renodx-engine: GetProcAddress D3DCompile failed");
        return false;
    }

    ID3DBlob* code = nullptr;
    ID3DBlob* errs = nullptr;
    HRESULT hr = g_D3DCompile(
        WORLD_PS_HLSL, std::strlen(WORLD_PS_HLSL),
        "world_ps_replacement.hlsl",
        nullptr, nullptr,
        "main", "ps_2_0",
        0, 0,
        &code, &errs);
    if (FAILED(hr) || !code) {
        char buf[400];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: D3DCompile FAILED hr=0x%08x errs=%s",
            (unsigned)hr,
            errs ? (const char*)errs->GetBufferPointer() : "(none)");
        reshade::log::message(reshade::log::level::error, buf);
        if (errs) errs->Release();
        return false;
    }

    g_replacement_bytecode.assign(
        (const uint8_t*)code->GetBufferPointer(),
        (const uint8_t*)code->GetBufferPointer() + code->GetBufferSize());
    code->Release();
    if (errs) errs->Release();

    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "renodx-engine: replacement world.ps compiled, %zu bytes ps_2_0 (red-tint test)",
        g_replacement_bytecode.size());
    reshade::log::message(reshade::log::level::info, buf);
    return true;
}

// ── pipeline tracking + replacement ─────────────────────────────────
std::mutex g_mu;
std::unordered_map<uint64_t, uint32_t> g_ps_pipeline_to_crc;
std::unordered_map<uint64_t, uint32_t> g_vs_pipeline_to_crc;
std::unordered_set<uint32_t> g_dumped_ps;

thread_local bool     t_z_enabled = true;
thread_local uint32_t t_vs_crc    = 0;
thread_local uint32_t t_ps_crc    = 0;

// addon_event::create_pipeline fires BEFORE the pipeline object is created.
// Returning true tells ReShade we modified the subobjects in-place; it
// will pass the modified set on to the underlying API.
bool on_create_pipeline(reshade::api::device*,
                        reshade::api::pipeline_layout,
                        uint32_t subobject_count,
                        const reshade::api::pipeline_subobject* subobjects) {
    if (g_replacement_bytecode.empty()) return false;

    bool modified = false;
    for (uint32_t i = 0; i < subobject_count; ++i) {
        const auto& s = subobjects[i];
        if (s.type != reshade::api::pipeline_subobject_type::pixel_shader) continue;
        auto* desc = static_cast<reshade::api::shader_desc*>(s.data);
        if (!desc || !desc->code || !desc->code_size) continue;

        uint32_t c = crc32(static_cast<const uint8_t*>(desc->code), desc->code_size);
        if (c == WORLD_PS_CRC) {
            desc->code      = g_replacement_bytecode.data();
            desc->code_size = g_replacement_bytecode.size();
            modified = true;
            uint64_t n = g_replacements_done.fetch_add(1, std::memory_order_relaxed) + 1;
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "renodx-engine: world.ps create_pipeline #%llu — substituted (%zu bytes)",
                (unsigned long long)n, g_replacement_bytecode.size());
            reshade::log::message(reshade::log::level::info, buf);
        }
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
        bool is_ps = s.type == reshade::api::pipeline_subobject_type::pixel_shader;
        bool is_vs = s.type == reshade::api::pipeline_subobject_type::vertex_shader;
        if (!is_ps && !is_vs) continue;
        auto* desc = static_cast<reshade::api::shader_desc*>(s.data);
        if (!desc || !desc->code || !desc->code_size) continue;
        uint32_t c = crc32(static_cast<const uint8_t*>(desc->code), desc->code_size);
        std::lock_guard<std::mutex> g(g_mu);
        if (is_ps) {
            g_ps_pipeline_to_crc[pipeline.handle] = c;
            if (g_dumped_ps.insert(c).second) {
                dump_ps_bytecode(c, desc->code, desc->code_size);
            }
        } else {
            g_vs_pipeline_to_crc[pipeline.handle] = c;
        }
    }
}

void on_bind_pipeline(reshade::api::command_list*,
                      reshade::api::pipeline_stage stages,
                      reshade::api::pipeline pipeline) {
    std::lock_guard<std::mutex> g(g_mu);
    if ((uint32_t)stages & (uint32_t)reshade::api::pipeline_stage::vertex_shader) {
        auto it = g_vs_pipeline_to_crc.find(pipeline.handle);
        t_vs_crc = (it != g_vs_pipeline_to_crc.end()) ? it->second : 0;
    }
    if ((uint32_t)stages & (uint32_t)reshade::api::pipeline_stage::pixel_shader) {
        auto it = g_ps_pipeline_to_crc.find(pipeline.handle);
        t_ps_crc = (it != g_ps_pipeline_to_crc.end()) ? it->second : 0;
    }
}

void on_bind_pipeline_states(reshade::api::command_list*,
                             uint32_t count,
                             const reshade::api::dynamic_state* states,
                             const uint32_t* values) {
    for (uint32_t i = 0; i < count; ++i)
        if (states[i] == reshade::api::dynamic_state::depth_enable)
            t_z_enabled = (values[i] != 0);
}

std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_world_draws{0};

bool count_world_draw(uint32_t /*verts*/) {
    if (t_ps_crc == WORLD_PS_CRC && t_z_enabled)
        g_world_draws.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool on_draw(reshade::api::command_list*, uint32_t v, uint32_t, uint32_t, uint32_t) {
    return count_world_draw(v);
}
bool on_draw_indexed(reshade::api::command_list*, uint32_t i, uint32_t, uint32_t, int32_t, uint32_t) {
    return count_world_draw(i);
}

void on_present(reshade::api::effect_runtime*) {
    uint64_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((f % 600) == 0) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: frames=%llu world.ps replacements=%llu world-draws=%llu",
            (unsigned long long)f,
            (unsigned long long)g_replacements_done.load(),
            (unsigned long long)g_world_draws.load());
        reshade::log::message(reshade::log::level::info, buf);
    }
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "neocron-renodx-engine";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Tier 4 m2 — replaces world.ps (CRC 0x2ccf5eb7) with HLSL-compiled red-tint test shader";

BOOL WINAPI DllMain(HINSTANCE hmod, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(hmod)) return FALSE;
            compile_replacement();   // populates g_replacement_bytecode if successful
            reshade::register_event<reshade::addon_event::create_pipeline>(on_create_pipeline);
            reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
            reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
            reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
            reshade::register_event<reshade::addon_event::draw>(on_draw);
            reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
            reshade::register_event<reshade::addon_event::reshade_present>(on_present);
            reshade::log::message(reshade::log::level::info,
                "renodx-engine: m2 (world.ps red-tint replacement) registered");
            break;
        case DLL_PROCESS_DETACH:
            reshade::unregister_event<reshade::addon_event::reshade_present>(on_present);
            reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
            reshade::unregister_event<reshade::addon_event::draw>(on_draw);
            reshade::unregister_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
            reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
            reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
            reshade::unregister_event<reshade::addon_event::create_pipeline>(on_create_pipeline);
            reshade::unregister_addon(hmod);
            if (g_d3dc) FreeLibrary(g_d3dc);
            break;
    }
    return TRUE;
}
