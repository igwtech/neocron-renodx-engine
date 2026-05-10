// neocron-renodx-engine — Tier 4 m3 PoC (v0.3.0):
// real per-pixel normal-mapped lighting.
//
// Loads ONE normal map (a brick from the AI corpus) at addon startup,
// binds it to D3D9 sampler 2 every frame, and the replacement world.ps
// HLSL samples it for Phong-style lighting against a hardcoded sun.
//
// Limitation acknowledged: a single normal map for ALL world surfaces
// means every wall/floor gets the same brick pattern, which is wrong
// where the underlying albedo isn't actual brick. This is a pipeline-
// validation PoC — the next milestone (m4) is per-texture lookup so the
// correct sibling _n.png binds for each albedo. Doable with a
// hash-based texture identification scheme; deferred for now because
// it's substantially more wiring.
//
// What you should see in-game vs vanilla:
//   - Brick walls in Plaza outer sectors get visible directional bump
//     (highlights/shadows on grout lines) — correct effect.
//   - Smooth concrete walls also get the brick bump — wrong effect, but
//     proves the per-pixel lighting math is wired.
//   - Tiled floors get the brick bump too — wrong but visible.

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <reshade.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

namespace {

// ── tunables ─────────────────────────────────────────────────────────
constexpr uint32_t WORLD_PS_CRC      = 0x2ccf5eb7u;
constexpr UINT     NORMAL_SAMPLER    = 5;   // moved off s2 — game/ReShade was clobbering s2
constexpr DWORD    NORMAL_TILING     = 4;   // multiply UV by this so the brick repeats N times across each surface
constexpr const char* DEFAULT_NORMAL_FILE = "neocron_default_normal.png";

// m3 v0.3.1 — full Phong with brick normal at sampler 5. Confirmed via debug
// dump that sampler 5 receives the bound texture correctly. Apparent "no
// effect" of v0.3 was actually Toddyhancer tonemap mangling the output —
// to see the bump cleanly, toggle ReShade Effects OFF (default key).
const char* WORLD_PS_HLSL = R"hlsl(
sampler2D samplerAlbedo   : register(s0);
sampler2D samplerLightmap : register(s1);
sampler2D samplerNormal   : register(s5);

static const float3 SUN_TS = float3(0.408, 0.408, 0.816);   // pre-normalised
static const float  TILING = 4.0;

float4 main(float2 uv : TEXCOORD0, float2 uv_lm : TEXCOORD1, float4 vcol : COLOR0) : COLOR {
    float4 albedo   = tex2D(samplerAlbedo,   uv);
    float4 lightmap = tex2D(samplerLightmap, uv_lm);

    // Sample tiled normal, decode RGB[0,1] → tangent normal[-1,1], normalise
    float3 n_ts = tex2D(samplerNormal, uv * TILING).rgb * 2.0 - 1.0;
    n_ts = normalize(n_ts);

    // Lambertian against fixed sun
    float NdotL = saturate(dot(n_ts, SUN_TS));
    float light = 0.40 + 0.60 * NdotL;

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

HMODULE        g_d3dc        = nullptr;
PFN_D3DCompile g_D3DCompile  = nullptr;
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
    if (!g_D3DCompile) return false;

    ID3DBlob* code = nullptr;
    ID3DBlob* errs = nullptr;
    HRESULT hr = g_D3DCompile(WORLD_PS_HLSL, std::strlen(WORLD_PS_HLSL),
        "world_ps_replacement.hlsl",
        nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &errs);
    if (FAILED(hr) || !code) {
        char buf[400];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: D3DCompile FAILED hr=0x%08x errs=%s",
            (unsigned)hr, errs ? (const char*)errs->GetBufferPointer() : "(none)");
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
        "renodx-engine: replacement world.ps compiled, %zu bytes ps_2_0 (m3 v0.3 normal-mapped Phong)",
        g_replacement_bytecode.size());
    reshade::log::message(reshade::log::level::info, buf);
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

// ── pipeline tracking ────────────────────────────────────────────────
std::mutex g_pipe_mu;
std::unordered_map<uint64_t, uint32_t> g_ps_pipeline_to_crc;
std::unordered_set<uint64_t>           g_substituted_ps_pipelines;
std::unordered_set<uint32_t>           g_dumped_ps;

thread_local bool     t_z_enabled = true;
thread_local bool     t_using_substituted_world_ps = false;

// ── ReShade callbacks ────────────────────────────────────────────────

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
            char buf[120];
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
        if (s.type != reshade::api::pipeline_subobject_type::pixel_shader) continue;
        auto* desc = static_cast<reshade::api::shader_desc*>(s.data);
        if (!desc || !desc->code || !desc->code_size) continue;
        uint32_t c = crc32(static_cast<const uint8_t*>(desc->code), desc->code_size);
        std::lock_guard<std::mutex> g(g_pipe_mu);
        g_ps_pipeline_to_crc[pipeline.handle] = c;
        // Did we just substitute this one? Mark by checking if the bytecode
        // matches our compiled replacement (same pointer + size).
        if (!g_replacement_bytecode.empty() &&
            desc->code == g_replacement_bytecode.data() &&
            desc->code_size == g_replacement_bytecode.size()) {
            g_substituted_ps_pipelines.insert(pipeline.handle);
        }
        if (g_dumped_ps.insert(c).second && c != WORLD_PS_CRC)
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
}

void on_bind_pipeline_states(reshade::api::command_list*,
                             uint32_t count,
                             const reshade::api::dynamic_state* states,
                             const uint32_t* values) {
    for (uint32_t i = 0; i < count; ++i)
        if (states[i] == reshade::api::dynamic_state::depth_enable)
            t_z_enabled = (values[i] != 0);
}

std::atomic<uint64_t> g_world_draws{0};

inline void bind_normal_for_draw(reshade::api::command_list* cmd) {
    if (!t_using_substituted_world_ps || !t_z_enabled || !g_default_normal) return;
    auto* dev = reinterpret_cast<IDirect3DDevice9*>(cmd->get_device()->get_native());
    if (!dev) return;
    dev->SetTexture(NORMAL_SAMPLER, g_default_normal);
    dev->SetSamplerState(NORMAL_SAMPLER, D3DSAMP_ADDRESSU,  D3DTADDRESS_WRAP);
    dev->SetSamplerState(NORMAL_SAMPLER, D3DSAMP_ADDRESSV,  D3DTADDRESS_WRAP);
    dev->SetSamplerState(NORMAL_SAMPLER, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(NORMAL_SAMPLER, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

    // Diagnostic: log GetTexture once to verify our bind took effect
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) == 0) {
        IDirect3DBaseTexture9* current = nullptr;
        dev->GetTexture(NORMAL_SAMPLER, &current);
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: SetTexture(%u, %p) → GetTexture returned %p (match=%d)",
            NORMAL_SAMPLER, (void*)g_default_normal, (void*)current,
            current == g_default_normal ? 1 : 0);
        reshade::log::message(reshade::log::level::info, buf);
        if (current) current->Release();
    }
    g_world_draws.fetch_add(1, std::memory_order_relaxed);
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

void on_present(reshade::api::effect_runtime* rt) {
    // Lazy-load normal on the first frame after the device is fully up.
    if (!g_default_normal && rt) {
        auto* dev = reinterpret_cast<IDirect3DDevice9*>(rt->get_device()->get_native());
        if (dev) load_default_normal(dev);
    }
    uint64_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((f % 1800) == 0) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: frames=%llu replacements=%llu world-draws=%llu normal-loaded=%d",
            (unsigned long long)f,
            (unsigned long long)g_replacements_done.load(),
            (unsigned long long)g_world_draws.load(),
            g_default_normal ? 1 : 0);
        reshade::log::message(reshade::log::level::info, buf);
    }
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "neocron-renodx-engine";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Tier 4 m3 PoC — normal-mapped Phong on world.ps via single brick normal at sampler 2";

BOOL WINAPI DllMain(HINSTANCE hmod, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(hmod)) return FALSE;
            compile_replacement();
            reshade::register_event<reshade::addon_event::create_pipeline>(on_create_pipeline);
            reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
            reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
            reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
            reshade::register_event<reshade::addon_event::draw>(on_draw);
            reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
            reshade::register_event<reshade::addon_event::reshade_present>(on_present);
            reshade::log::message(reshade::log::level::info,
                "renodx-engine: m3 v0.3 (normal-mapped Phong) registered");
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
            if (g_default_normal) { g_default_normal->Release(); g_default_normal = nullptr; }
            if (g_d3dc) FreeLibrary(g_d3dc);
            break;
    }
    return TRUE;
}
