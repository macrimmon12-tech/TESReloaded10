// ReShadeIntegration.cpp
// Registers NVR as a ReShade addon so ReShade effects fire at the correct pipeline
// point (after NVR post-process, before UI) with a correctly unified depth buffer.
//
// Framework.h is force-included by the project, supplying all NVR types.
// reshade.hpp is included here only, so ReShade types stay out of the wider codebase.
//
// Depth: CombineDepth.fx outputs G32R32F where .y is projection-space depth in main-
// camera space for all pixels (world + viewmodel, viewmodel remapped). We copy that
// channel to an R32F surface and hand it to ReShade via update_texture_bindings("DEPTH"),
// giving RTGI/RTAO correct, unified depth under both native D3D9 and DXVK.

// Prevent assert macro redefinition (Framework.h redefines assert to static_assert).
#ifdef assert
#undef assert
#endif
// Prevent reshade_overlay.hpp from activating its ImGui version check — we only
// need the core addon and effect_runtime API, not the overlay helpers.
#ifdef IMGUI_VERSION_NUM
#undef IMGUI_VERSION_NUM
#endif
#include <reshade.hpp>

using namespace reshade;
using namespace reshade::api;

// ── module state ─────────────────────────────────────────────────────────────

static bool             s_registered      = false;
static HMODULE          s_hModule         = nullptr;
static effect_runtime*  s_runtime         = nullptr;
static command_list*    s_cmdList         = nullptr;
static resource_view    s_sceneRTV        = {};
static resource_view    s_sceneRTVsrgb    = {};
static bool             s_validPass       = false;

static std::vector<std::string> s_savedTechniques;

static IDirect3DTexture9*  s_depthTexture    = nullptr;
static IDirect3DSurface9*  s_depthSurface    = nullptr;
static ID3DXEffect*        s_depthCopyEffect = nullptr;

// ── depth copy shader ─────────────────────────────────────────────────────────
// Reads the G channel (.y) of the G32R32F CombineDepth output, which stores
// projection-space depth remapped to main-camera near/far for all pixels.
// This is what ReShade expects for GetLinearizedDepth() to work correctly.

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

// ── ReShade event callbacks ───────────────────────────────────────────────────

static void OnBeginEffects(
    effect_runtime* runtime, command_list* cmd_list,
    resource_view rtv, resource_view rtv_srgb)
{
    s_runtime      = runtime;
    s_cmdList      = cmd_list;
    s_sceneRTV     = rtv;
    s_sceneRTVsrgb = rtv_srgb;
    s_savedTechniques.clear();

    if (s_validPass) return;

    // Suppress all techniques so they don't fire at ReShade's normal present-time slot.
    runtime->enumerate_techniques(nullptr,
        [](effect_runtime* rt, effect_technique tech) {
            char name[256];
            rt->get_technique_name(tech, name);
            if (rt->get_technique_state(tech)) {
                s_savedTechniques.emplace_back(name);
                rt->set_technique_state(tech, false);
            }
        });
}

static void OnFinishEffects(
    effect_runtime* runtime, command_list* /*cmd_list*/,
    resource_view /*rtv*/, resource_view /*rtv_srgb*/)
{
    if (s_validPass) {
        s_validPass = false;
        return;
    }

    // Restore techniques that were active before suppression.
    runtime->enumerate_techniques(nullptr,
        [](effect_runtime* rt, effect_technique tech) {
            char name[256];
            rt->get_technique_name(tech, name);
            bool restore = std::find(
                s_savedTechniques.begin(), s_savedTechniques.end(), name)
                != s_savedTechniques.end();
            rt->set_technique_state(tech, restore);
        });
}

static void OnBindRenderTargets(
    command_list* /*cmd_list*/,
    uint32_t count, const resource_view* rtvs,
    resource_view /*dsv*/)
{
    if (count > 0)
        s_sceneRTV = rtvs[0];
}

// ── public interface ──────────────────────────────────────────────────────────

void ReShadeIntegration::Initialize() {
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ReShadeIntegration::Initialize),
        &s_hModule);

    if (!reshade::register_addon(s_hModule)) {
        Logger::Log("ReShadeIntegration: ReShade not present, integration disabled");
        return;
    }

    s_registered = true;
    Logger::Log("ReShadeIntegration: registered as ReShade addon");

    reshade::register_event<addon_event::reshade_begin_effects>(OnBeginEffects);
    reshade::register_event<addon_event::reshade_finish_effects>(OnFinishEffects);
    reshade::register_event<addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);

    IDirect3DDevice9* Device = TheRenderManager->device;

    HRESULT hr = Device->CreateTexture(
        TheRenderManager->width, TheRenderManager->height,
        1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
        D3DPOOL_DEFAULT, &s_depthTexture, nullptr);

    if (FAILED(hr)) {
        Logger::Log("ReShadeIntegration: CreateTexture R32F failed (hr=0x%08X) - depth integration disabled", hr);
        Shutdown();
        return;
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
        Shutdown();
        return;
    }

    if (errors) errors->Release();
    Logger::Log("ReShadeIntegration: initialized - depth integration active");
}

void ReShadeIntegration::Shutdown() {
    if (s_depthCopyEffect) { s_depthCopyEffect->Release(); s_depthCopyEffect = nullptr; }
    if (s_depthSurface)    { s_depthSurface->Release();    s_depthSurface    = nullptr; }
    if (s_depthTexture)    { s_depthTexture->Release();    s_depthTexture    = nullptr; }

    if (s_registered) {
        reshade::unregister_addon(s_hModule);
        s_registered = false;
    }
}

void ReShadeIntegration::RenderEffects(IDirect3DSurface9* /*renderTarget*/) {
    if (!s_registered || !s_runtime || !s_cmdList)
        return;
    if (!s_depthTexture || !s_depthSurface || !s_depthCopyEffect)
        return;
    if (!TheTextureManager->CombinedDepthTexture)
        return;

    IDirect3DDevice9* Device = TheRenderManager->device;

    // Save and override render target for the depth copy pass.
    IDirect3DSurface9* savedRT = nullptr;
    Device->GetRenderTarget(0, &savedRT);
    Device->SetRenderTarget(0, s_depthSurface);

    // Bind CombineDepth output (G32R32F) to sampler 0 and run the copy pass.
    Device->SetTexture(0, TheTextureManager->CombinedDepthTexture);
    Device->SetStreamSource(0, TheShaderManager->FrameVertex, 0, sizeof(FrameVS));
    Device->SetFVF(FrameFVF);

    UINT passes = 0;
    s_depthCopyEffect->Begin(&passes, 0);
    s_depthCopyEffect->BeginPass(0);
    Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    s_depthCopyEffect->EndPass();
    s_depthCopyEffect->End();

    // Restore the scene render target before handing off to ReShade.
    Device->SetRenderTarget(0, savedRT);
    if (savedRT) savedRT->Release();

    // Override ReShade's depth binding with our unified R32F depth.
    const resource_view depthRV = { reinterpret_cast<uint64_t>(s_depthTexture) };
    s_runtime->update_texture_bindings("DEPTH", depthRV, depthRV);

    // Fire ReShade effects at this pipeline point (post-NVR, pre-UI).
    s_validPass = true;
    s_runtime->render_effects(s_cmdList, s_sceneRTV, s_sceneRTVsrgb);
}
