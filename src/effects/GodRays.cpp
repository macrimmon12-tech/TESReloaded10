#include "GodRays.h"
#include "../core/GpuProfiler.h"

void GodRaysEffect::UpdateConstants() {
	Constants.Data.z = std::lerp(nightMult, dayMult, TheShaderManager->GameState.transitionCurve);
	Constants.Ray.w = rayVisibility * sunGlareEnabled ? TheShaderManager->ShaderConst.sunGlare : 1.0;
}

void GodRaysEffect::UpdateSettings(){
	dayMult = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "DayMultiplier");
	nightMult = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "NightMultiplier");
	sunGlareEnabled = TheSettingManager->GetSettingI("Shaders.GodRays.Main", "SunGlareEnabled");

	Constants.Ray.x = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "RayIntensity");
	Constants.Ray.y = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "RayLength");
	Constants.Ray.z = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "RayDensity");
	rayVisibility = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "RayVisibility");
	Constants.RayColor.x = TheSettingManager->GetSettingF("Shaders.GodRays.Coloring", "RayR");
	Constants.RayColor.y = TheSettingManager->GetSettingF("Shaders.GodRays.Coloring", "RayG");
	Constants.RayColor.z = TheSettingManager->GetSettingF("Shaders.GodRays.Coloring", "RayB");
	Constants.RayColor.w = TheSettingManager->GetSettingF("Shaders.GodRays.Coloring", "Saturate");
	Constants.Data.x = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "LightShaftPasses");
	Constants.Data.y = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "Luminance");
	Constants.Data.w = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "TimeEnabled");
}

void GodRaysEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_GodRaysRay", &Constants.Ray);
	TheShaderManager->RegisterConstant("TESR_GodRaysRayColor", &Constants.RayColor);
	TheShaderManager->RegisterConstant("TESR_GodRaysData", &Constants.Data);
}

void GodRaysEffect::RegisterTextures() {
	const int width = (TheRenderManager->width + 1) / 2;
	const int height = (TheRenderManager->height + 1) / 2;
	TheTextureManager->InitTexture("NVR_GodRaysBuffer0", &raysTexture[0], &raysSurface[0], width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("NVR_GodRaysBuffer1", &raysTexture[1], &raysSurface[1], width, height, D3DFMT_A16B16G16R16F);
	const int lowWidth = (TheRenderManager->width + 3) / 4;
	const int lowHeight = (TheRenderManager->height + 3) / 4;
	TheTextureManager->InitTexture("NVR_GodRaysBufferLow0", &raysTextureLow[0], &raysSurfaceLow[0], lowWidth, lowHeight, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("NVR_GodRaysBufferLow1", &raysTextureLow[1], &raysSurfaceLow[1], lowWidth, lowHeight, D3DFMT_A16B16G16R16F);
}

bool GodRaysEffect::ShouldRender() {
	return TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater && TheShaderManager->GameState.dayLight > 0.5;
}

/*
* Five half-resolution (or, with GodRaysLowRes, quarter-resolution) passes ping-pong between two
* dedicated targets, then a single full-resolution combine. The shaders work in [0, 1] UVs and take
* the target size from NVR_GodRaysLayout, so both sizes use the same technique. Relies on
* RenderedSurface already holding the current scene, which every preceding effect guarantees by
* copying its result there. Returns false without touching the render target if the dedicated
* path is unavailable.
*/
bool GodRaysEffect::RenderDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface) {
	if (dedicatedRenderFailed) return false;
	const bool lowRes = TheSettingManager->SettingsMain.Main.GodRaysLowRes &&
		raysTextureLow[0] && raysTextureLow[1] && raysSurfaceLow[0] && raysSurfaceLow[1];
	IDirect3DTexture9** textures = lowRes ? raysTextureLow : raysTexture;
	IDirect3DSurface9** surfaces = lowRes ? raysSurfaceLow : raysSurface;
	const UINT divisor = lowRes ? 4 : 2;
	D3DXHANDLE technique = Effect->GetTechniqueByName("DedicatedGodRays");
	D3DXHANDLE layoutHandle = Effect->GetParameterByName(NULL, "NVR_GodRaysLayout");
	D3DXTECHNIQUE_DESC description = {};
	D3DVIEWPORT9 original = {};
	D3DSURFACE_DESC target = {}, scratch = {}, rays = {};
	if (!technique || !layoutHandle || FAILED(Effect->GetTechniqueDesc(technique, &description)) ||
		description.Passes != 6 || !RenderedSurface || !textures[0] || !textures[1] ||
		!surfaces[0] || !surfaces[1] ||
		FAILED(Device->GetViewport(&original)) || FAILED(RenderTarget->GetDesc(&target)) ||
		FAILED(RenderedSurface->GetDesc(&scratch)) || FAILED(surfaces[0]->GetDesc(&rays)) ||
		original.X || original.Y ||
		original.Width != target.Width || original.Height != target.Height ||
		scratch.Width != target.Width || scratch.Height != target.Height ||
		rays.Width != (target.Width + divisor - 1) / divisor || rays.Height != (target.Height + divisor - 1) / divisor ||
		target.MultiSampleType != D3DMULTISAMPLE_NONE)
		return false;

	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = chain.Owns(RenderTarget, RenderedSurface);
	IDirect3DSurface9* finalTarget = chained ? chain.Output() : RenderTarget;
	D3DVIEWPORT9 reduced = original;
	reduced.Width = rays.Width;
	reduced.Height = rays.Height;
	D3DXVECTOR4 layout((float)reduced.Width / original.Width,
		(float)reduced.Height / original.Height, 1.0f / reduced.Width, 1.0f / reduced.Height);
	IDirect3DSurface9* depthSurface = nullptr;
	DWORD oldScissorEnabled = FALSE;
	HRESULT result = Device->GetRenderState(D3DRS_SCISSORTESTENABLE, &oldScissorEnabled);
	if (SUCCEEDED(result)) result = Device->GetDepthStencilSurface(&depthSurface);
	if (SUCCEEDED(result)) result = Device->SetDepthStencilSurface(nullptr);
	if (FAILED(result)) {
		if (depthSurface) depthSurface->Release();
		dedicatedRenderFailed = true;
		return false;
	}

	// pass -> destination: 0 SkyMask->A, 1 LightMask->B, 2 Blur1->A, 3 Blur2->B, 4 Blur3->A, 5 Combine->RT
	Effect->SetTechnique(technique);
	SetCT();
	Effect->SetVector(layoutHandle, &layout);
	UINT passes = 0;
	result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		static GpuTimer passTimers[6] = { GpuTimer("  GR sky mask (half)"), GpuTimer("  GR light mask (half)"),
			GpuTimer("  GR blur 1 (half)"), GpuTimer("  GR blur 2 (half)"), GpuTimer("  GR blur 3 (half)"),
			GpuTimer("  GR combine (full)") };
		for (UINT p = 0; p < passes && SUCCEEDED(result); ++p) {
			GpuProfileScope gpuPass(passTimers[p < 6 ? p : 5], Device);
			const bool combine = p == passes - 1;
			Device->SetTexture(5, nullptr);
			result = Device->SetRenderTarget(0, combine ? finalTarget : surfaces[p & 1]);
			if (SUCCEEDED(result)) result = Device->SetViewport(combine ? &original : &reduced);
			if (SUCCEEDED(result)) result = Device->SetRenderState(D3DRS_SCISSORTESTENABLE, combine ? oldScissorEnabled : FALSE);
			if (FAILED(result)) break;
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			if (p > 0) Device->SetTexture(5, textures[combine ? 0 : (p - 1) & 1]);
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
		}
		Effect->End();
	}
	Device->SetTexture(5, nullptr);
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetViewport(&original);
	Device->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScissorEnabled);
	Device->SetDepthStencilSurface(depthSurface);
	if (depthSurface) depthSurface->Release();
	if (SUCCEEDED(result)) {
		if (chained) chain.Commit();
		else result = Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	}
	if (FAILED(result)) {
		// RenderedSurface is only written by the final copy, so it still holds the input.
		Device->StretchRect(RenderedSurface, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		dedicatedRenderFailed = true;
		Logger::Log("Dedicated god rays failed (%08lx); using packed path until restart.", result);
		return false;
	}
	static bool reported[2] = {};
	if (!reported[lowRes]) {
		Logger::Log("Dedicated god rays active: %ux%u A16B16G16R16F ping-pong, six passes%s.",
			reduced.Width, reduced.Height, lowRes ? " (GodRaysLowRes)" : "");
		reported[lowRes] = true;
	}
	return true;
}

void GodRaysEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
	IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { renderTime = 0; return; }

	{
		auto timer = TimeLogger();
		if (RenderDedicated(Device, RenderTarget, RenderedSurface)) {
			renderTime = timer.LogTime("GodRays::DedicatedRender");
			return;
		}
	}

	D3DXHANDLE technique = Effect->GetTechnique(techniqueIndex);
	D3DXTECHNIQUE_DESC description = {};
	D3DSURFACE_DESC target = {}, scratch = {};
	DWORD oldScissorEnabled = FALSE;
	RECT oldScissor = {};
	if (packedRenderFailed || !technique || !RenderedSurface || !SourceBuffer ||
		FAILED(Effect->GetTechniqueDesc(technique, &description)) || description.Passes != 6 ||
		FAILED(RenderTarget->GetDesc(&target)) || FAILED(RenderedSurface->GetDesc(&scratch)) ||
		target.Width != scratch.Width || target.Height != scratch.Height ||
		FAILED(Device->GetRenderState(D3DRS_SCISSORTESTENABLE, &oldScissorEnabled)) ||
		FAILED(Device->GetScissorRect(&oldScissor))) {
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}

	auto timer = TimeLogger();
	TheShaderManager->Chain.Sync(); // the packed path reads the render target itself
	RECT packed = {0, 0, (LONG)(target.Width / 2), (LONG)(target.Height / 2)};
	D3DRECT packedClear = {packed.left, packed.top, packed.right, packed.bottom};
	HRESULT result = Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_LINEAR);
	if (SUCCEEDED(result)) result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) SetCT();
	UINT passes = 0;
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		for (UINT p = 0; p < passes && SUCCEEDED(result); ++p) {
			const bool combine = p == passes - 1;
			if (combine) {
				result = Device->SetScissorRect(&oldScissor);
				if (SUCCEEDED(result)) result = Device->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScissorEnabled);
				if (SUCCEEDED(result) && ClearRenderTarget)
					result = Device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
			}
			else {
				result = Device->SetScissorRect(&packed);
				if (SUCCEEDED(result)) result = Device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
				if (SUCCEEDED(result) && ClearRenderTarget)
					result = Device->Clear(1, &packedClear, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
			}
			if (FAILED(result)) break;
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
			if (SUCCEEDED(result)) result = Device->StretchRect(RenderTarget, combine ? NULL : &packed,
				RenderedSurface, combine ? NULL : &packed, D3DTEXF_LINEAR);
		}
		Effect->End();
	}
	Device->SetScissorRect(&oldScissor);
	Device->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScissorEnabled);

	if (FAILED(result)) {
		Device->StretchRect(SourceBuffer, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		Device->StretchRect(SourceBuffer, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
		packedRenderFailed = true;
		Logger::Log("Packed god rays failed (%08lx); using legacy path until restart.", result);
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("Packed god rays active: %ux%u intermediate region.", packed.right, packed.bottom);
		reported = true;
	}
	renderTime = timer.LogTime("GodRays::PackedRender");
}
