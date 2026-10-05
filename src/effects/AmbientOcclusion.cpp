#include "AmbientOcclusion.h"
#include "../core/GpuProfiler.h"

void AmbientOcclusionEffect::UpdateConstants() {
}

void AmbientOcclusionEffect::RegisterConstants() {
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionAOData"] = &Constants.AOData;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionData"] = &Constants.Data;
}

void AmbientOcclusionEffect::RegisterTextures() {
	const int width = (TheRenderManager->width + 1) / 2;
	const int height = (TheRenderManager->height + 1) / 2;
	TheTextureManager->InitTexture("NVR_AOBuffer0", &aoTexture[0], &aoSurface[0], width, height, D3DFMT_G16R16F);
	TheTextureManager->InitTexture("NVR_AOBuffer1", &aoTexture[1], &aoSurface[1], width, height, D3DFMT_G16R16F);
	const int lowWidth = (TheRenderManager->width + 3) / 4;
	const int lowHeight = (TheRenderManager->height + 3) / 4;
	TheTextureManager->InitTexture("NVR_AOBufferLow0", &aoTextureLow[0], &aoSurfaceLow[0], lowWidth, lowHeight, D3DFMT_G16R16F);
	TheTextureManager->InitTexture("NVR_AOBufferLow1", &aoTextureLow[1], &aoSurfaceLow[1], lowWidth, lowHeight, D3DFMT_G16R16F);
}

bool AmbientOcclusionEffect::UseLowRes() const {
	return TheSettingManager->SettingsMain.Main.AOLowRes &&
		aoTextureLow[0] && aoTextureLow[1] && aoSurfaceLow[0] && aoSurfaceLow[1];
}

void AmbientOcclusionEffect::UpdateSettings() {
	const char* sectionName = TheShaderManager->GameState.isExterior?"Shaders.AmbientOcclusion.Exteriors":"Shaders.AmbientOcclusion.Interiors";

	Constants.Enabled = TheSettingManager->GetSettingI(sectionName, "Enabled");
	Constants.AOData.x = TheSettingManager->GetSettingF(sectionName, "Samples");
	Constants.AOData.y = TheSettingManager->GetSettingF(sectionName, "StrengthMultiplier");
	Constants.AOData.z = TheSettingManager->GetSettingF(sectionName, "ClampStrength");
	Constants.AOData.w = TheSettingManager->GetSettingF(sectionName, "Range");
	Constants.Data.x = TheSettingManager->GetSettingF(sectionName, "AngleBias");
	Constants.Data.y = TheSettingManager->GetSettingF(sectionName, "LumThreshold");
	Constants.Data.z = TheSettingManager->GetSettingF(sectionName, "BlurDropThreshold");
	Constants.Data.w = TheSettingManager->GetSettingF(sectionName, "BlurRadiusMultiplier");
}

bool AmbientOcclusionEffect::ShouldRender() {
	return Constants.Enabled && !bNVAOLoaded;
}

void AmbientOcclusionEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
	IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { renderTime = 0; return; }
	// combineOnly reapplies this frame's deferred estimate, so it keeps that estimate's resolution.
	if (!combineOnly) lastLowRes = UseLowRes();
	IDirect3DTexture9* const* textures = lastLowRes ? aoTextureLow : aoTexture;
	IDirect3DSurface9* const* surfaces = lastLowRes ? aoSurfaceLow : aoSurface;
	const UINT divisor = lastLowRes ? 4 : 2;
	D3DXHANDLE technique = Effect->GetTechniqueByName("DedicatedAO");
	D3DXHANDLE layoutHandle = Effect->GetParameterByName(NULL, "NVR_AOLayout");
	D3DXTECHNIQUE_DESC description = {};
	D3DVIEWPORT9 original = {};
	D3DSURFACE_DESC target = {}, scratch = {}, aoTarget = {};
	IDirect3DSurface9* depthSurface = nullptr;
	// Older/custom effect files keep the legacy path. Never guess their pass layout.
	if (packedAOFailed || !technique || !layoutHandle || FAILED(Effect->GetTechniqueDesc(technique, &description)) ||
		description.Passes != 4 || !RenderedSurface || !SourceBuffer || !textures[0] || !textures[1] ||
		!surfaces[0] || !surfaces[1] ||
		FAILED(Device->GetViewport(&original)) || FAILED(RenderTarget->GetDesc(&target)) ||
		FAILED(RenderedSurface->GetDesc(&scratch)) || FAILED(surfaces[0]->GetDesc(&aoTarget)) ||
		original.X || original.Y ||
		original.Width != target.Width || original.Height != target.Height ||
		scratch.Width != target.Width || scratch.Height != target.Height ||
		aoTarget.Width != (target.Width + divisor - 1) / divisor || aoTarget.Height != (target.Height + divisor - 1) / divisor ||
		aoTarget.Format != D3DFMT_G16R16F ||
		target.MultiSampleType != D3DMULTISAMPLE_NONE) {
		// Let the caller restore deferred shadows before running legacy AO.
		if (deferCombine) return;
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}
	auto timer = TimeLogger();
	// Under the frame chain the combine writes the chain's spare texture, which then becomes the
	// current image; otherwise it writes the render target and is copied back as before.
	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = chain.Owns(RenderTarget, RenderedSurface);
	IDirect3DSurface9* finalTarget = chained ? chain.Output() : RenderTarget;
	D3DVIEWPORT9 reduced = original;
	reduced.Width = aoTarget.Width;
	reduced.Height = aoTarget.Height;
	D3DXVECTOR4 layout((float)reduced.Width / original.Width,
		(float)reduced.Height / original.Height, 1.0f / reduced.Width, 1.0f / reduced.Height);
	// The combine reads the scene through the TESR_SourceBuffer slot (s2). RenderedSurface already
	// equals the render target when AO starts and nothing here writes it before the final copy,
	// so bind it there instead of copying the full frame into SourceBuffer.
	IDirect3DTexture9* scene = TheTextureManager->RenderedTexture;
	HRESULT result = scene ? Device->GetDepthStencilSurface(&depthSurface) : E_FAIL;
	if (SUCCEEDED(result)) result = Device->SetDepthStencilSurface(nullptr);
	if (FAILED(result)) {
		if (depthSurface) depthSurface->Release();
		return;
	}
	Effect->SetTechnique(technique);
	SetCT();
	Effect->SetVector(layoutHandle, &layout);
	UINT passes = 0;
	result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		const UINT firstPass = combineOnly && passes ? passes - 1 : 0;
		const UINT endPass = deferCombine && passes ? passes - 1 : passes;
		for (UINT p = firstPass; p < endPass && SUCCEEDED(result); ++p) {
			const bool combine = p == passes - 1;
			Device->SetTexture(5, nullptr);
			IDirect3DSurface9* destination = combine ? finalTarget : surfaces[p == 1 ? 1 : 0];
			result = Device->SetRenderTarget(0, destination);
			if (SUCCEEDED(result)) result = Device->SetViewport(combine ? &original : &reduced);
			if (FAILED(result)) break;
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			if (p == 1) Device->SetTexture(5, textures[0]);
			else if (p == 2) Device->SetTexture(5, textures[1]);
			else if (combine) Device->SetTexture(5, textures[0]);
			Device->SetTexture(2, scene); // TESR_SourceBuffer slot
			// Sub-pass timings (nested inside "Ambient occlusion") to direct further AO work.
			static GpuTimer passTimers[4] = { GpuTimer("  AO estimate (half)"), GpuTimer("  AO blur X (half)"),
				GpuTimer("  AO blur Y (half)"), GpuTimer("  AO combine (full)") };
			{
				GpuProfileScope gpu(passTimers[p < 4 ? p : 3], Device);
				result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			}
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
		}
		Effect->End();
	}
	Device->SetTexture(5, nullptr);
	Device->SetTexture(2, TheTextureManager->SourceTexture);
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetViewport(&original);
	Device->SetDepthStencilSurface(depthSurface);
	if (depthSurface) depthSurface->Release();
	static bool reported[2] = {};
	auto reportActive = [&]() {
		if (reported[lastLowRes]) return;
		Logger::Log("Dedicated AO active: %ux%u G16R16F ping-pong%s.", reduced.Width, reduced.Height,
			lastLowRes ? " (AOLowRes)" : "");
		reported[lastLowRes] = true;
	};
	if (SUCCEEDED(result) && deferCombine) {
		// Nothing was written to the frame; the fog pass applies ResultTexture().
		deferredReady = true;
		reportActive();
		renderTime = timer.LogTime("AmbientOcclusion::Deferred");
		return;
	}
	if (SUCCEEDED(result)) {
		if (chained) chain.Commit();
		else result = Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	}
	if (FAILED(result)) {
		// RenderedSurface is only written by the final copy, so it still holds the input.
		Device->StretchRect(RenderedSurface, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		packedAOFailed = true;
		Logger::Log("Packed AO failed (%08lx); using legacy AO until restart.", result);
		if (deferCombine) return;
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}
	reportActive();
	renderTime = timer.LogTime("AmbientOcclusion::PackedAO");
}
