#include "../core/GpuProfiler.h"

void SunShadowsEffect::SetCT() {
	EffectRecord::SetCT();

	// Change shadow atlas sampler state if anisotropy is supposed to be used.
	int anisotropy = TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps.Anisotropy;
	if (anisotropy) {
		TheRenderManager->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
		TheRenderManager->SetSamplerState(1, D3DSAMP_MAXANISOTROPY, anisotropy);
	}
}

void SunShadowsEffect::RegisterTextures() {
	TheTextureManager->InitTexture("NVR_ShadowPassScratch0", &scratchTexture[0], &scratchSurface[0],
		TheRenderManager->width, TheRenderManager->height, D3DFMT_G16R16);
	TheTextureManager->InitTexture("NVR_ShadowPassScratch1", &scratchTexture[1], &scratchSurface[1],
		TheRenderManager->width, TheRenderManager->height, D3DFMT_G16R16);
}

/*
* The stock path renders every pass into TESR_PointShadowBuffer while the same passes sample it
* (the contact-shadow blurs read neighbouring texels), a read/write feedback loop with undefined
* results. Route the passes through two scratch targets instead -- buffer -> A -> B -> ... ->
* buffer -- so each pass reads a texture that is not bound for writing, and the last pass still
* lands in TESR_PointShadowBuffer without an extra copy.
*/
void SunShadowsEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
	UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { renderTime = 0; return; }

	IDirect3DTexture9* shadowTexture = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowPassTexture;
	IDirect3DSurface9* shadowSurface = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowPassSurface;
	D3DXHANDLE technique = Effect->GetTechnique(techniqueIndex);
	D3DXTECHNIQUE_DESC description = {};
	D3DSURFACE_DESC target = {}, scratch = {};
	if (pingPongFailed || !technique || RenderTarget != shadowSurface || !shadowTexture ||
		!scratchTexture[0] || !scratchTexture[1] || !scratchSurface[0] || !scratchSurface[1] ||
		FAILED(Effect->GetTechniqueDesc(technique, &description)) || description.Passes < 2 ||
		FAILED(RenderTarget->GetDesc(&target)) || FAILED(scratchSurface[0]->GetDesc(&scratch)) ||
		target.Width != scratch.Width || target.Height != scratch.Height || target.Format != scratch.Format) {
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}

	auto timer = TimeLogger();
	HRESULT result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) SetCT();
	UINT passes = 0;
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		// Per-pass GPU timers nested in the caller's "Sun contact shadows": march, horizontal blur,
		// vertical blur (+ intensity), and the deferred shadow apply of the four-pass technique.
		static GpuTimer passTimers[4] = { GpuTimer("  Contact march"), GpuTimer("  Contact blur H"),
			GpuTimer("  Contact blur V"), GpuTimer("  Contact shadow apply") };
		IDirect3DTexture9* source = shadowTexture;
		for (UINT p = 0; p < passes && SUCCEEDED(result); ++p) {
			GpuProfileScope gpuPass(passTimers[p < 4 ? p : 3], Device);
			const bool last = p == passes - 1;
			IDirect3DSurface9* destination = last ? shadowSurface : scratchSurface[p & 1];
			Device->SetTexture(3, nullptr); // TESR_PointShadowBuffer slot
			result = Device->SetRenderTarget(0, destination);
			if (FAILED(result)) break;
			if (ClearRenderTarget) Device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			Device->SetTexture(3, source);
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
			source = last ? shadowTexture : scratchTexture[p & 1];
		}
		Effect->End();
	}
	Device->SetTexture(3, nullptr);
	Device->SetRenderTarget(0, RenderTarget);
	if (FAILED(result)) {
		pingPongFailed = true;
		Logger::Log("Sun shadow ping-pong failed (%08lx); using the in-place path until restart.", result);
		return;
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("Contact shadows use scratch ping-pong (%u passes, no feedback loop).", passes);
		reported = true;
	}
	renderTime = timer.LogTime("SunShadows::PingPong");
}
