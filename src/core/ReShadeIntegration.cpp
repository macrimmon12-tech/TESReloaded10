// ReShadeIntegration.cpp
// Registers NVR as a ReShade addon and supplies a unified depth buffer.
//
// Framework.h is force-included by the project, supplying all NVR types.
// reshade.hpp is included here only, so ReShade types stay out of the wider codebase.
//
// Depth: CombineDepth.fx outputs G32R32F where .y is projection-space depth in main-
// camera space for all pixels (world + viewmodel, viewmodel remapped). We copy that
// channel to an R32F surface and hand it to ReShade via update_texture_bindings("DEPTH"),
// giving RTGI/RTAO correct, unified depth under both native D3D9 and DXVK.
//
// Registration is deferred: RenderEffects() retries each frame until ReShade's module
// appears (Vulkan ReShade loads as a global layer after NVR's Initialize() runs).
// register_addon is called directly (bypassing reshade.hpp's wrapper) with version
// probing so we work against release builds whose RESHADE_API_VERSION < our headers.

// Prevent assert macro redefinition (Framework.h redefines assert to static_assert).
#ifdef assert
#undef assert
#endif
// Prevent reshade_overlay.hpp from activating its ImGui version check.
#ifdef IMGUI_VERSION_NUM
#undef IMGUI_VERSION_NUM
#endif
#include <reshade.hpp>

using namespace reshade;
using namespace reshade::api;

// ── module state ─────────────────────────────────────────────────────────────

static HMODULE          s_hModule         = nullptr;
static HMODULE          s_reshadeModule   = nullptr;
static bool             s_registered      = false;
static bool             s_gaveUp          = false;
static int              s_retryFrame      = 0;

static IDirect3DTexture9*  s_depthTexture    = nullptr;
static IDirect3DSurface9*  s_depthSurface    = nullptr;
static ID3DXEffect*        s_depthCopyEffect = nullptr;

// ── depth copy shader ─────────────────────────────────────────────────────────
// Reads the G channel (.y) of the G32R32F CombineDepth output, which stores
// projection-space depth remapped to main-camera near/far for all pixels.

static const char kDepthCopyFX[] = R"(
sampler2D CombinedDepth : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP;
    MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE;
};

struct VSOUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSIN  { float4 pos : POSITION0; float2 uv : TEXCOORD0; };

VSOUT VS(VSIN IN) {
    VSOUT OUT = (VSOUT)0;
    OUT.pos = IN.pos;
    OUT.uv  = IN.uv;
    return OUT;
}

float4 PS(VSOUT IN) : COLOR0 {
    return tex2D(CombinedDepth, IN.uv).gggg;
}

technique T0 {
    pass P0 {
        VertexShader = compile vs_3_0 VS();
        PixelShader  = compile ps_3_0 PS();
    }
}
)";

// ── ReShade event callback ────────────────────────────────────────────────────
// Fires right before ReShade renders its effect pipeline each frame.
// We update the DEPTH texture binding with our unified depth so RTGI/RTAO
// shaders sample the correct depth for both world and viewmodel pixels.

static void OnBeginEffects(
    effect_runtime* runtime, command_list* /*cmd_list*/,
    resource_view /*rtv*/, resource_view /*rtv_srgb*/)
{
    if (!s_depthTexture) return;
    const resource_view depthRV = { reinterpret_cast<uint64_t>(s_depthTexture) };
    runtime->update_texture_bindings("DEPTH", depthRV, depthRV);
}

// ── diagnostic ───────────────────────────────────────────────────────────────

static void DiagnoseModules() {
    HMODULE modules[1024]; DWORD num = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &num)) {
        Logger::Log("ReShadeIntegration: K32EnumProcessModules failed (err=%u)", GetLastError());
        return;
    }

    DWORD count = num / sizeof(HMODULE);
    Logger::Log("ReShadeIntegration: scanning %u loaded modules for ReShade", count);

    for (DWORD i = 0; i < count; ++i) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(modules[i], path, MAX_PATH);

        bool hasReg   = GetProcAddress(modules[i], "ReShadeRegisterAddon")   != nullptr;
        bool hasUnreg = GetProcAddress(modules[i], "ReShadeUnregisterAddon") != nullptr;

        char lower[MAX_PATH] = {};
        for (int j = 0; path[j] && j < MAX_PATH - 1; ++j)
            lower[j] = static_cast<char>(tolower(static_cast<unsigned char>(path[j])));

        if (hasReg || hasUnreg || strstr(lower, "reshade"))
            Logger::Log("ReShadeIntegration:   [%u] %s  RegisterAddon=%s UnregisterAddon=%s",
                i, path, hasReg ? "YES" : "no", hasUnreg ? "YES" : "no");
    }
}

// ── lazy registration ─────────────────────────────────────────────────────────

static bool TryRegister() {
    // Find the ReShade module.
    HMODULE reshadeModule = nullptr;
    {
        HMODULE modules[1024]; DWORD num = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &num))
            return false;
        for (DWORD i = 0; i < num / sizeof(HMODULE); ++i) {
            if (GetProcAddress(modules[i], "ReShadeRegisterAddon") &&
                GetProcAddress(modules[i], "ReShadeUnregisterAddon")) {
                reshadeModule = modules[i];
                break;
            }
        }
    }
    if (!reshadeModule) return false;

    // Call ReShadeRegisterAddon directly, probing downward from our compiled
    // RESHADE_API_VERSION until the installed DLL accepts one.
    auto fnRegister = reinterpret_cast<bool(WINAPI*)(HMODULE, uint32_t)>(
        GetProcAddress(reshadeModule, "ReShadeRegisterAddon"));

    uint32_t acceptedVersion = 0;
    for (uint32_t v = RESHADE_API_VERSION; v >= 1 && !acceptedVersion; --v)
        if (fnRegister(s_hModule, v)) acceptedVersion = v;

    if (!acceptedVersion) {
        Logger::Log("ReShadeIntegration: ReShadeRegisterAddon rejected all API versions 1..%u", RESHADE_API_VERSION);
        return false;
    }

    if (acceptedVersion != RESHADE_API_VERSION)
        Logger::Log("ReShadeIntegration: WARNING - negotiated API version %u (headers v%u)",
            acceptedVersion, RESHADE_API_VERSION);

    s_reshadeModule = reshadeModule;

    reshade::register_event<addon_event::reshade_begin_effects>(OnBeginEffects);

    IDirect3DDevice9* Device = TheRenderManager->device;

    HRESULT hr = Device->CreateTexture(
        TheRenderManager->width, TheRenderManager->height,
        1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
        D3DPOOL_DEFAULT, &s_depthTexture, nullptr);

    if (FAILED(hr)) {
        Logger::Log("ReShadeIntegration: CreateTexture R32F failed (hr=0x%08X) - depth integration disabled", hr);
        auto fnUnreg = reinterpret_cast<void(WINAPI*)(HMODULE)>(
            GetProcAddress(reshadeModule, "ReShadeUnregisterAddon"));
        if (fnUnreg) fnUnreg(s_hModule);
        s_reshadeModule = nullptr;
        return false;
    }

    s_depthTexture->GetSurfaceLevel(0, &s_depthSurface);

    ID3DXBuffer* errors = nullptr;
    hr = D3DXCreateEffect(
        Device,
        kDepthCopyFX, static_cast<UINT>(sizeof(kDepthCopyFX) - 1),
        nullptr, nullptr, 0, nullptr,
        &s_depthCopyEffect, &errors);

    if (FAILED(hr)) {
        if (errors) {
            Logger::Log("ReShadeIntegration: depth copy shader compile error: %s",
                static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        Logger::Log("ReShadeIntegration: shader compile failed (hr=0x%08X) - depth integration disabled", hr);
        auto fnUnreg = reinterpret_cast<void(WINAPI*)(HMODULE)>(
            GetProcAddress(reshadeModule, "ReShadeUnregisterAddon"));
        if (fnUnreg) fnUnreg(s_hModule);
        s_reshadeModule = nullptr;
        if (s_depthSurface) { s_depthSurface->Release(); s_depthSurface = nullptr; }
        if (s_depthTexture) { s_depthTexture->Release(); s_depthTexture = nullptr; }
        return false;
    }

    if (errors) errors->Release();
    s_registered = true;
    Logger::Log("ReShadeIntegration: registered with API v%u - depth integration active", acceptedVersion);
    return true;
}

// ── public interface ──────────────────────────────────────────────────────────

void ReShadeIntegration::Initialize() {
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ReShadeIntegration::Initialize),
        &s_hModule);

    Logger::Log("ReShadeIntegration: deferred registration scheduled");
}

void ReShadeIntegration::Shutdown() {
    if (s_depthCopyEffect) { s_depthCopyEffect->Release(); s_depthCopyEffect = nullptr; }
    if (s_depthSurface)    { s_depthSurface->Release();    s_depthSurface    = nullptr; }
    if (s_depthTexture)    { s_depthTexture->Release();    s_depthTexture    = nullptr; }

    if (s_registered && s_reshadeModule) {
        auto fnUnreg = reinterpret_cast<void(WINAPI*)(HMODULE)>(
            GetProcAddress(s_reshadeModule, "ReShadeUnregisterAddon"));
        if (fnUnreg) fnUnreg(s_hModule);
        s_reshadeModule = nullptr;
        s_registered = false;
    }
}

void ReShadeIntegration::RenderEffects(IDirect3DSurface9* /*renderTarget*/) {
    if (!s_registered) {
        if (s_gaveUp) return;

        ++s_retryFrame;

        if (s_retryFrame > 300) {
            DiagnoseModules();
            Logger::Log("ReShadeIntegration: ReShade not detected after 300 frames - integration disabled");
            s_gaveUp = true;
            return;
        }

        if (!TryRegister()) return;
    }

    if (!s_depthTexture || !s_depthSurface || !s_depthCopyEffect) return;
    if (!TheTextureManager->CombinedDepthTexture) return;

    IDirect3DDevice9* Device = TheRenderManager->device;

    // Copy the G channel of CombineDepth's G32R32F output into our R32F texture.
    // OnBeginEffects will bind this to ReShade's DEPTH semantic before effects fire.
    IDirect3DSurface9* savedRT = nullptr;
    Device->GetRenderTarget(0, &savedRT);
    Device->SetRenderTarget(0, s_depthSurface);

    Device->SetTexture(0, TheTextureManager->CombinedDepthTexture);
    Device->SetStreamSource(0, TheShaderManager->FrameVertex, 0, sizeof(FrameVS));
    Device->SetFVF(FrameFVF);

    UINT passes = 0;
    s_depthCopyEffect->Begin(&passes, 0);
    s_depthCopyEffect->BeginPass(0);
    Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    s_depthCopyEffect->EndPass();
    s_depthCopyEffect->End();

    Device->SetRenderTarget(0, savedRT);
    if (savedRT) savedRT->Release();
}
