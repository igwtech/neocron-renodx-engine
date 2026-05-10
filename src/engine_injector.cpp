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
#include <reshade.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
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
constexpr uint32_t WORLD_PS_CRC          = 0x2ccf5eb7u;
constexpr UINT     NORMAL_SAMPLER        = 5;   // moved off s2 — game/ReShade was clobbering s2
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
constexpr const char* HD_CORPUS_DIR     = "gfx_normals\\";
constexpr const char* STOCK_CORPUS_DIR  = "gfx_normals_stock\\";

constexpr size_t HASH_SAMPLE_BYTES = 4096;  // must match build-hash-index.py
constexpr size_t NORMAL_CACHE_MAX  = 256;   // cap LRU; ~50 MB for typical 256² normals

// Pixel-shader constant register for our params — picked to NOT collide with
// game's world.ps which uses c0..c7 for color correction etc.
constexpr UINT  PARAMS_PS_REG = 20;   // c20 = bump_amp, light_min, light_max, debug_mode
constexpr UINT  SUN_PS_REG    = 21;   // c21 = sun direction (tangent space)

// Tunables (default values; hotkeys mutate at runtime)
struct Tunables {
    float bump_amp   = 5.0f;   // F3-/F4+
    float light_min  = 0.20f;  // F5/F6 narrows / widens
    float light_max  = 1.50f;
    float debug_mode = 0.0f;   // F2 cycles 0..3
    float sun_x      = 0.408f, sun_y = 0.408f, sun_z = 0.816f;  // pre-normalised
    void reset() { *this = Tunables{}; }
};
std::atomic<float> g_bump_amp{5.0f};
std::atomic<float> g_light_min{0.20f};
std::atomic<float> g_light_max{1.50f};
std::atomic<int>   g_debug_mode{0};
// Sun direction is fixed for now (no F-key tweak yet); kept in defaults.
constexpr float SUN_X = 0.408f, SUN_Y = 0.408f, SUN_Z = 0.816f;

// m4 v0.4.3 — runtime-tunable normal mapping with debug viz modes.
//
// Pixel shader constants we use (game's world.ps uses c0..c7 for its own
// stuff like colorCorrection, so we go to c20+ for safety):
//   c20.x = BUMP_AMP        (XY tilt amplification, 0.5..15)
//   c20.y = LIGHT_MIN       (light value at NdotL=0, default 0.2)
//   c20.z = LIGHT_MAX       (light value at NdotL=1, default 1.5)
//   c20.w = DEBUG_MODE      (0=normal, 1=raw normal RGB, 2=NdotL gray, 3=albedo only)
//   c21.xyz = SUN_TS        (sun direction in tangent space, normalised)
//
// Hotkeys (handled in on_present via effect_runtime::is_key_pressed):
//   F2 = cycle debug mode
//   F3 = bump amp -1   F4 = bump amp +1
//   F5 = light range narrower   F6 = wider
//   F11 = reset to defaults
const char* WORLD_PS_HLSL = R"hlsl(
sampler2D samplerAlbedo   : register(s0);
sampler2D samplerLightmap : register(s1);
sampler2D samplerNormal   : register(s5);

float4 g_params : register(c20);  // x=bump_amp y=light_min z=light_max w=debug_mode
float4 g_sun    : register(c21);  // xyz=tangent-space sun

float4 main(float2 uv : TEXCOORD0, float2 uv_lm : TEXCOORD1, float4 vcol : COLOR0) : COLOR {
    float4 albedo   = tex2D(samplerAlbedo,   uv);
    float4 lightmap = tex2D(samplerLightmap, uv_lm);

    float3 n_raw = tex2D(samplerNormal, uv).rgb * 2.0 - 1.0;
    float3 n_ts  = float3(n_raw.xy * g_params.x, n_raw.z);
    n_ts = normalize(n_ts);

    float NdotL = saturate(dot(n_ts, g_sun.xyz));
    float light = lerp(g_params.y, g_params.z, NdotL);
    float4 lit  = saturate(albedo * lightmap * 2.0 * light);

    // Debug modes — branch is on a uniform (w), free in ps_2_0 static branching
    float mode = g_params.w;
    if (mode > 0.5 && mode < 1.5) return float4(n_raw * 0.5 + 0.5, 1.0); // raw normal
    if (mode > 1.5 && mode < 2.5) return float4(NdotL.xxx, 1.0);          // NdotL grayscale
    if (mode > 2.5)                return albedo;                          // albedo only
    return lit;
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

// ── m4 per-texture normal lookup ─────────────────────────────────────
//
// Two layers of mapping:
//   index:  uint64_t hash      → relative path  (loaded once from .txt)
//   live:   uint64_t resource  → uint64_t hash  (built at init_resource)
//   cache:  uint64_t hash      → IDirect3DTexture9*  (lazy on first draw use)

std::mutex  g_index_mu;
// Map value is the FULL relative path (corpus_root prepended at load time),
// so get_or_load_normal doesn't need to know which index it came from.
std::unordered_map<uint64_t, std::string> g_hash_to_path;

std::mutex  g_resource_mu;
std::unordered_map<uint64_t, uint64_t>    g_resource_to_hash;

std::mutex  g_normal_cache_mu;
std::unordered_map<uint64_t, IDirect3DTexture9*> g_normal_cache;
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
    char line[1100];
    while (std::fgets(line, sizeof(line), f)) {
        char hex[24] = {0};
        char rel[1024] = {0};
        if (std::sscanf(line, "%23s %1023[^\r\n]", hex, rel) != 2) continue;
        char* endp = nullptr;
        uint64_t h = std::strtoull(hex, &endp, 16);
        if (!h || endp == hex) continue;
        ++loaded;
        // Insert only if hash isn't already present (preserves precedence).
        std::string full = std::string(corpus_root) + rel;
        // Normalize separators to backslash for Win32 / Wine.
        for (auto& c : full) if (c == '/') c = '\\';
        if (g_hash_to_path.emplace(h, std::move(full)).second) ++kept;
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

const std::string* lookup_normal_path(uint64_t hash) {
    std::lock_guard<std::mutex> g(g_index_mu);
    auto it = g_hash_to_path.find(hash);
    return it != g_hash_to_path.end() ? &it->second : nullptr;
}

// Lazy load a normal map for the given hash. Returns nullptr if not in
// index, file missing, or D3D9 upload failed (also caches the miss).
IDirect3DTexture9* get_or_load_normal(IDirect3DDevice9* dev, uint64_t hash) {
    {
        std::lock_guard<std::mutex> g(g_normal_cache_mu);
        auto it = g_normal_cache.find(hash);
        if (it != g_normal_cache.end()) return it->second;   // may be nullptr (cached miss)
    }

    const std::string* rel = lookup_normal_path(hash);
    if (!rel) {
        std::lock_guard<std::mutex> g(g_normal_cache_mu);
        g_normal_cache.emplace(hash, nullptr);
        return nullptr;
    }

    // Index entries are full paths (corpus root prepended at load time +
    // separators normalized). Use as-is.
    const std::string& full = *rel;

    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(full.c_str(), &w, &h, &ch, 4);
    if (!px) {
        char buf[400];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: stbi_load FAILED for normal %s (hash %016llx): %s",
            full.c_str(), (unsigned long long)hash, stbi_failure_reason());
        reshade::log::message(reshade::log::level::warning, buf);
        std::lock_guard<std::mutex> g(g_normal_cache_mu);
        g_normal_cache.emplace(hash, nullptr);
        return nullptr;
    }

    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = dev->CreateTexture((UINT)w, (UINT)h, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    if (FAILED(hr) || !tex) {
        stbi_image_free(px);
        std::lock_guard<std::mutex> g(g_normal_cache_mu);
        g_normal_cache.emplace(hash, nullptr);
        return nullptr;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
        tex->Release();
        stbi_image_free(px);
        std::lock_guard<std::mutex> g(g_normal_cache_mu);
        g_normal_cache.emplace(hash, nullptr);
        return nullptr;
    }
    // RGBA → BGRA (D3DFMT_A8R8G8B8 byte order in memory)
    for (int y = 0; y < h; ++y) {
        uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
        const unsigned char* src = px + y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x*4 + 0] = src[x*4 + 2];
            dst[x*4 + 1] = src[x*4 + 1];
            dst[x*4 + 2] = src[x*4 + 0];
            dst[x*4 + 3] = src[x*4 + 3];
        }
    }
    tex->UnlockRect(0);
    stbi_image_free(px);

    {
        std::lock_guard<std::mutex> g(g_normal_cache_mu);
        // Crude cap: if too many entries, evict an arbitrary live one.
        // LRU not worth the complexity for a 256-entry budget on a 2004 game.
        if (g_normal_cache.size() >= NORMAL_CACHE_MAX) {
            for (auto it = g_normal_cache.begin(); it != g_normal_cache.end(); ++it) {
                if (it->second) {
                    it->second->Release();
                    g_normal_cache.erase(it);
                    break;
                }
            }
        }
        g_normal_cache[hash] = tex;
    }

    uint64_t n = g_lazy_loaded.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n % 50) == 0) {
        char buf[400];
        std::snprintf(buf, sizeof(buf),
            "renodx-engine: lazy-loaded normal #%llu hash=%016llx %dx%d → %s",
            (unsigned long long)n, (unsigned long long)hash, w, h, rel->c_str());
        reshade::log::message(reshade::log::level::info, buf);
    }
    return tex;
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

inline void bind_normal_for_draw(reshade::api::command_list* cmd) {
    if (!t_using_substituted_world_ps || !t_z_enabled) return;
    auto* dev = reinterpret_cast<IDirect3DDevice9*>(cmd->get_device()->get_native());
    if (!dev) return;

    // Resolve which normal to bind from whatever the game just put on s0.
    IDirect3DTexture9* normal = nullptr;
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
            // First time we've seen this albedo bound — hash it now.
            // hash_d3d9_texture returns 0 if LockRect fails (e.g.
            // POOL_DEFAULT without a system copy), which we cache as
            // "miss" to avoid re-trying every draw.
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
        if (hash) normal = get_or_load_normal(dev, hash);
        albedo->Release();
    }

    bool per_tex = normal != nullptr;
    if (!normal) normal = g_default_normal;
    if (!normal) return;   // nothing to bind, leave shader sampling whatever's there

    dev->SetTexture(NORMAL_SAMPLER, normal);
    dev->SetSamplerState(NORMAL_SAMPLER, D3DSAMP_ADDRESSU,  D3DTADDRESS_WRAP);
    dev->SetSamplerState(NORMAL_SAMPLER, D3DSAMP_ADDRESSV,  D3DTADDRESS_WRAP);
    dev->SetSamplerState(NORMAL_SAMPLER, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(NORMAL_SAMPLER, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

    // Push tunables to pixel-shader constants. Cheap (4 floats per draw).
    float params[4] = {
        g_bump_amp.load(std::memory_order_relaxed),
        g_light_min.load(std::memory_order_relaxed),
        g_light_max.load(std::memory_order_relaxed),
        (float)g_debug_mode.load(std::memory_order_relaxed)
    };
    dev->SetPixelShaderConstantF(PARAMS_PS_REG, params, 1);
    float sun[4] = { SUN_X, SUN_Y, SUN_Z, 0.0f };
    dev->SetPixelShaderConstantF(SUN_PS_REG, sun, 1);

    g_world_draws.fetch_add(1, std::memory_order_relaxed);
    if (per_tex) {
        uint64_t n = g_world_draws_per_tex_hit.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "renodx-engine: FIRST per-texture normal bound (resource=%p tex=%p)",
                (void*)albedo, (void*)normal);
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

inline void poll_hotkeys(reshade::api::effect_runtime* rt) {
    // Win32 VK_* values — defined as macros in winuser.h, use directly.
    if (rt->is_key_pressed(VK_F2)) {
        g_debug_mode.store((g_debug_mode.load() + 1) % 4, std::memory_order_relaxed);
        log_tunables("F2 cycle debug");
    }
    if (rt->is_key_pressed(VK_F3)) {
        g_bump_amp.store(std::max(0.5f,  g_bump_amp.load() - 1.0f), std::memory_order_relaxed);
        log_tunables("F3 amp -");
    }
    if (rt->is_key_pressed(VK_F4)) {
        g_bump_amp.store(std::min(15.0f, g_bump_amp.load() + 1.0f), std::memory_order_relaxed);
        log_tunables("F4 amp +");
    }
    if (rt->is_key_pressed(VK_F5)) {
        g_light_min.store(std::min(0.95f, g_light_min.load() + 0.05f), std::memory_order_relaxed);
        g_light_max.store(std::max(1.05f, g_light_max.load() - 0.05f), std::memory_order_relaxed);
        log_tunables("F5 light narrow");
    }
    if (rt->is_key_pressed(VK_F6)) {
        g_light_min.store(std::max(0.0f,  g_light_min.load() - 0.05f), std::memory_order_relaxed);
        g_light_max.store(std::min(3.0f,  g_light_max.load() + 0.05f), std::memory_order_relaxed);
        log_tunables("F6 light widen");
    }
    if (rt->is_key_pressed(VK_F11)) {
        g_bump_amp.store(5.0f);   g_light_min.store(0.20f);
        g_light_max.store(1.50f); g_debug_mode.store(0);
        log_tunables("F11 reset");
    }
}

void on_present(reshade::api::effect_runtime* rt) {
    // Lazy-load normal on the first frame after the device is fully up.
    if (!g_default_normal && rt) {
        auto* dev = reinterpret_cast<IDirect3DDevice9*>(rt->get_device()->get_native());
        if (dev) load_default_normal(dev);
    }
    if (rt) poll_hotkeys(rt);
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
            reshade::log::message(reshade::log::level::info,
                "renodx-engine: m4 v0.4.3 (per-texture lookup + tunable HLSL + debug viz) registered");
            log_tunables("init defaults");
            reshade::log::message(reshade::log::level::info,
                "renodx-engine: hotkeys — F2=cycle debug viz (0=lit/1=raw normals/2=NdotL gray/3=albedo only), "
                "F3/F4=bump amp -/+, F5/F6=light range narrow/widen, F11=reset");
            break;
        case DLL_PROCESS_DETACH:
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
                for (auto& kv : g_normal_cache) if (kv.second) kv.second->Release();
                g_normal_cache.clear();
            }
            if (g_d3dc) FreeLibrary(g_d3dc);
            break;
    }
    return TRUE;
}
