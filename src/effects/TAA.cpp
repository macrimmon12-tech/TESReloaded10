#include <algorithm>

#include "TAA.h"

void TAAEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_TAAData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_TAAPrevProjection", &Constants.PrevProjection);
	TheShaderManager->RegisterConstant("TESR_TAACameraDelta", &Constants.CameraDelta);
	TheShaderManager->RegisterConstant("TESR_TAAPrevViewTransform", (D3DXVECTOR4*)&Constants.PrevViewTransform);
}

void TAAEffect::RegisterTextures() {
	int width = TheRenderManager->width;
	int height = TheRenderManager->height;

	TheTextureManager->InitTexture("TESR_TAAResolveBuffer", &Textures.ResolveTexture, &Textures.ResolveSurface, width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_TAAHistoryBuffer", &Textures.HistoryTexture, &Textures.HistorySurface, width, height, D3DFMT_A16B16G16R16F);
}

void TAAEffect::UpdateSettings() {
	// A key absent from the toml reads as 0. Both settings would be useless at 0 -- no history at
	// all, or a box collapsed onto the neighbourhood mean -- so 0 falls back to the default rather
	// than silently turning the effect into a blur or a no-op for anyone without the defaults file.
	float weight = TheSettingManager->GetSettingF("Shaders.TAA.Main", "HistoryWeight");
	Settings.HistoryWeight = weight > 0.0f ? std::clamp(weight, 0.5f, 0.97f) : 0.9f;

	float gamma = TheSettingManager->GetSettingF("Shaders.TAA.Main", "ClipGamma");
	Settings.ClipGamma = gamma > 0.0f ? std::clamp(gamma, 0.5f, 2.5f) : 1.0f;
}

void TAAEffect::UpdateConstants() {
	Constants.Data.x = Settings.HistoryWeight;
	Constants.Data.y = Settings.ClipGamma;
}

// Record the camera that rendered the frame now in the history buffer, for next frame's
// reprojection. Called after the resolve, never before it: at that point TheRenderManager holds
// the camera the frame was drawn with, because RenderEffects rebuilds it right before this effect.
void TAAEffect::RememberCamera() {
	Constants.PrevViewTransform = TheRenderManager->viewMatrix;
	Constants.PrevProjection = D3DXVECTOR4(TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22, 0.0f, 0.0f);
	prevCameraPosition = TheRenderManager->CameraPosition;
}

bool TAAEffect::DrawTechnique(const char* TechniqueName) {
	D3DXHANDLE technique = Effect->GetTechniqueByName(TechniqueName);
	if (!technique) {
		Logger::Log("[ERROR] TAA : technique %s not found", TechniqueName);
		return false;
	}

	Effect->SetTechnique(technique);
	UINT passes;
	Effect->Begin(&passes, 0);
	Effect->BeginPass(0);
	TheRenderManager->device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	Effect->EndPass();
	Effect->End();
	return true;
}

void TAAEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || Effect == nullptr || !ShouldRender() || !Textures.ResolveSurface || !Textures.HistorySurface) {
		// Whatever is in the history now will be stale, or somewhere else entirely, by the time
		// this runs again.
		historyValid = false;
		renderTime = 0.0f;
		return;
	}

	auto timer = TimeLogger();

	// A new cell means a loading screen or a teleport: last frame's history shows another place.
	if (TheShaderManager->GameState.isCellChanged) historyValid = false;

	// With no usable history, make last frame's camera this frame's. The reprojection becomes the
	// identity, and the shader ignores the history anyway because Data.z is 0.
	if (!historyValid) RememberCamera();

	D3DXVECTOR4 delta = TheRenderManager->CameraPosition - prevCameraPosition;
	Constants.CameraDelta = D3DXVECTOR4(delta.x, delta.y, delta.z, 0.0f);
	Constants.Data.z = historyValid ? 1.0f : 0.0f;

	if (SourceBuffer) Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_NONE);
	SetCT();

	Device->SetRenderTarget(0, Textures.ResolveSurface);
	bool resolved = DrawTechnique("Resolve");
	if (resolved) Device->StretchRect(Textures.ResolveSurface, NULL, Textures.HistorySurface, NULL, D3DTEXF_NONE);

	Device->SetRenderTarget(0, RenderTarget);
	if (resolved && DrawTechnique("Output")) {
		if (RenderedSurface) Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
		RememberCamera();
		historyValid = true;
	}
	else {
		historyValid = false;
	}

	renderTime = timer.LogTime("EffectRecord::Render TAA");
}
