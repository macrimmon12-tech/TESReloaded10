// ReShadeIntegration.cpp
// Registers NVR as a ReShade addon and supplies a unified depth buffer.
//
// Framework.h is force-included by the project, supplying all NVR types.
// reshade.hpp is included here only, so ReShade types stay out of the wider codebase.
//
// Depth strategy differs by backend:
//
//   Native D3D9 (TheRenderManager->DXVK == false):
//     CombineDepth.fx outputs G32R32F where .y is projection-space depth.
//     We copy that channel to an R32F surface and hand it to ReShade via
//     update_texture_bindings("DEPTH") in the reshade_begin_effects callback.
//     Under native D3D9, resource_view.handle == IDirect3DBaseTexture9*, which
//     is exactly what ReShade's D3D9 backend expects.
//
//   DXVK / Vulkan ReShade (TheRenderManager->DXVK == true):
//     Under DXVK, ReShade operates as a Vulkan implicit layer.  resource_view
//     handles are VkImageView pointers, not D3D9 COM pointers — passing a D3D9
//     pointer to update_texture_bindings causes an access violation inside
//     Vulkan.  Instead we write our combined depth directly into the game's
//     hardware depth-stencil surface (TheTextureManager->DepthSurface) via a
//     pixel shader that outputs the DEPTH semantic.  DXVK's built-in depth-blit
//     mechanism copies that Z image for ReShade's DEPTH semantic at present
//     time, so RTGI/RTAO see the correct unified depth without any vtable calls
//     on effect_runtime.
//
// Registration is deferred: RenderEffects() retries each frame until ReShade's
// module appears.  register_addon is called directly (bypassing reshade.hpp's
// wrapper) with version probing so we work against release builds whose
// RESHADE_API_VERSION < our headers.

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

// Native D3D9 path only: R32F texture passed to ReShade via update_texture_bindings.
static IDirect3DTexture9*  s_depthTexture    = nullptr;
static IDirect3DSurface9*  s_depthSurface    = nullptr;
static ID3DXEffect*        s_depthCopyEffect = nullptr;

// DXVK path only: writes combined depth to the hardware depth-stencil surface.
static ID3DXEffect*        s_depthWriteEffect = nullptr;

// ── depth copy shader (native D3D9 path) ─────────────────────────────────────
// Reads the G channel of the G32R32F CombineDepth output into an R32F surface.
// The R32F surface is then bound to ReShade's DEPTH semantic via
// update_texture_bindings in OnBeginEffects.

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

// ── depth write shader (DXVK path) ───────────────────────────────────────────
// Reads the G channel of the G32R32F CombineDepth output and writes it as the
// hardware depth value for every pixel.  Render states (ZEnable, ZWriteEnable,
// ZFunc=ALWAYS, ColorWriteEnable=0) are set programmatically around the draw
// call so that only the depth buffer is modified — the colour render target is
// left untouched.

static const char kDepthWriteFX[] = R"(
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

float4 PS(VSOUT IN, out float oDepth : DEPTH) : COLOR0 {
    oDepth = tex2D(CombinedDepth, IN.uv).g;
    return 0;
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
//
// Native D3D9 only: bind our R32F depth texture to the DEPTH semantic so
// RTGI/RTAO shaders sample unified depth for both world and viewmodel pixels.
// Under native D3D9, resource_view.handle is an IDirect3DBaseTexture9* — exactly
// what ReShade's D3D9 backend expects.
//
// DXVK: depth has already been written into the hardware Z buffer in
// RenderEffects(); DXVK's depth-blit mechanism copies it for ReShade at present
// time, so nothing needs to be done here.

static void OnBeginEffects(
    effect_runtime* runtime, command_list* /*cmd_list*/,
    resource_view /*rtv*/, resource_view /*rtv_srgb*/)
{
    if (TheRenderManager->DXVK) return;
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
    ID3DXBuffer* errors = nullptr;
    HRESULT hr;

    auto cleanup = [&](const char* msg) {
        if (errors) { Logger::Log("ReShadeIntegration: %s: %s", msg,
            static_cast<const char*>(errors->GetBufferPointer())); errors->Release(); }
        Logger::Log("ReShadeIntegration: %s (hr=0x%08X) - depth integration disabled", msg, hr);
        auto fnUnreg = reinterpret_cast<void(WINAPI*)(HMODULE)>(
            GetProcAddress(reshadeModule, "ReShadeUnregisterAddon"));
        if (fnUnreg) fnUnreg(s_hModule);
        s_reshadeModule = nullptr;
        if (s_depthWriteEffect) { s_depthWriteEffect->Release(); s_depthWriteEffect = nullptr; }
        if (s_depthSurface)     { s_depthSurface->Release();     s_depthSurface     = nullptr; }
        if (s_depthTexture)     { s_depthTexture->Release();     s_depthTexture     = nullptr; }
    };

    if (TheRenderManager->DXVK) {
        // DXVK path: compile the depth-write shader only.
        // Depth is injected into the hardware Z buffer in RenderEffects();
        // no R32F texture or ReShade resource_view needed.
        hr = D3DXCreateEffect(
            Device,
            kDepthWriteFX, static_cast<UINT>(sizeof(kDepthWriteFX) - 1),
            nullptr, nullptr, 0, nullptr,
            &s_depthWriteEffect, &errors);

        if (FAILED(hr)) {
            cleanup("depth write shader compile failed");
            return false;
        }
        if (errors) { errors->Release(); errors = nullptr; }
    } else {
        // Native D3D9 path: create an R32F render target and compile the depth
        // copy shader.  update_texture_bindings in OnBeginEffects will bind it.
        hr = Device->CreateTexture(
            TheRenderManager->width, TheRenderManager->height,
            1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
            D3DPOOL_DEFAULT, &s_depthTexture, nullptr);

        if (FAILED(hr)) {
            cleanup("CreateTexture R32F failed");
            return false;
        }

        s_depthTexture->GetSurfaceLevel(0, &s_depthSurface);

        hr = D3DXCreateEffect(
            Device,
            kDepthCopyFX, static_cast<UINT>(sizeof(kDepthCopyFX) - 1),
            nullptr, nullptr, 0, nullptr,
            &s_depthCopyEffect, &errors);

        if (FAILED(hr)) {
            cleanup("depth copy shader compile failed");
            return false;
        }
        if (errors) { errors->Release(); errors = nullptr; }
    }

    s_registered = true;
    Logger::Log("ReShadeIntegration: registered with API v%u - depth integration active (DXVK=%s)",
        acceptedVersion, TheRenderManager->DXVK ? "true" : "false");
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
    if (s_depthWriteEffect) { s_depthWriteEffect->Release(); s_depthWriteEffect = nullptr; }
    if (s_depthCopyEffect)  { s_depthCopyEffect->Release();  s_depthCopyEffect  = nullptr; }
    if (s_depthSurface)     { s_depthSurface->Release();     s_depthSurface     = nullptr; }
    if (s_depthTexture)     { s_depthTexture->Release();     s_depthTexture     = nullptr; }

    if (s_registered && s_reshadeModule) {
        auto fnUnreg = reinterpret_cast<void(WINAPI*)(HMODULE)>(
            GetProcAddress(s_reshadeModule, "ReShadeUnregisterAddon"));
        if (fnUnreg) fnUnreg(s_hModule);
        s_reshadeModule = nullptr;
        s_registered = false;
    }
}

void ReShadeIntegration::RenderEffects(IDirect3DSurface9* renderTarget) {
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

    if (!TheTextureManager->CombinedDepthTexture) return;

    IDirect3DDevice9* Device = TheRenderManager->device;

    if (TheRenderManager->DXVK) {
        // ── DXVK path ────────────────────────────────────────────────────────
        // Write the combined depth (world + viewmodel remapped) into the game's
        // hardware depth-stencil surface so that DXVK's depth-blit mechanism
        // copies it for ReShade's DEPTH semantic at present time.
        //
        // We use ZFunc=ALWAYS so every pixel is unconditionally overwritten with
        // our combined value regardless of what the hardware Z holds.
        // ColorWriteEnable=0 ensures the colour render target is not touched.
        if (!s_depthWriteEffect) return;
        if (!TheTextureManager->DepthSurface) return;

        // Save state.
        IDirect3DSurface9* savedRT = nullptr;
        IDirect3DSurface9* savedDS = nullptr;
        Device->GetRenderTarget(0, &savedRT);
        Device->GetDepthStencilSurface(&savedDS);

        DWORD savedZE, savedZW, savedZF, savedCWE;
        Device->GetRenderState(D3DRS_ZENABLE,         &savedZE);
        Device->GetRenderState(D3DRS_ZWRITEENABLE,    &savedZW);
        Device->GetRenderState(D3DRS_ZFUNC,           &savedZF);
        Device->GetRenderState(D3DRS_COLORWRITEENABLE,&savedCWE);

        // Use the frame output surface as the colour RT to guarantee that its
        // dimensions match the depth stencil.  ColorWriteEnable=0 ensures
        // nothing is written to it.
        Device->SetRenderTarget(0, renderTarget);
        Device->SetDepthStencilSurface(TheTextureManager->DepthSurface);
        Device->SetRenderState(D3DRS_ZENABLE,          TRUE);
        Device->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
        Device->SetRenderState(D3DRS_ZFUNC,            D3DCMP_ALWAYS);
        Device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);

        Device->SetTexture(0, TheTextureManager->CombinedDepthTexture);
        Device->SetStreamSource(0, TheShaderManager->FrameVertex, 0, sizeof(FrameVS));
        Device->SetFVF(FrameFVF);

        UINT passes = 0;
        s_depthWriteEffect->Begin(&passes, 0);
        s_depthWriteEffect->BeginPass(0);
        Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        s_depthWriteEffect->EndPass();
        s_depthWriteEffect->End();

        static bool s_loggedDepthWrite = false;
        if (!s_loggedDepthWrite) {
            Logger::Log("ReShadeIntegration: DXVK depth write executed (DepthSurface=%p CombinedDepth=%p)",
                TheTextureManager->DepthSurface, TheTextureManager->CombinedDepthTexture);
            s_loggedDepthWrite = true;
        }

        // Restore state.
        Device->SetRenderState(D3DRS_COLORWRITEENABLE, savedCWE);
        Device->SetRenderState(D3DRS_ZFUNC,            savedZF);
        Device->SetRenderState(D3DRS_ZWRITEENABLE,     savedZW);
        Device->SetRenderState(D3DRS_ZENABLE,          savedZE);
        Device->SetDepthStencilSurface(savedDS);
        Device->SetRenderTarget(0, savedRT);
        if (savedDS) savedDS->Release();
        if (savedRT) savedRT->Release();

    } else {
        // ── Native D3D9 path ─────────────────────────────────────────────────
        // Copy the G channel of CombineDepth's G32R32F output into our R32F
        // texture.  OnBeginEffects binds this to ReShade's DEPTH semantic before
        // effects fire.
        if (!s_depthTexture || !s_depthSurface || !s_depthCopyEffect) return;

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
}
