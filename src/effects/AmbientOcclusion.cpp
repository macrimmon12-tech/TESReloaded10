#include "AmbientOcclusion.h"

void AmbientOcclusionEffect::UpdateConstants() {
}

void AmbientOcclusionEffect::RegisterConstants() {
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionAOData"] = &Constants.AOData;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionData"] = &Constants.Data;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionFold"] = &Constants.Fold;
}

void AmbientOcclusionEffect::RegisterTextures() {
	// One sixteen bit channel: the occlusion is a single number per pixel.
	TheTextureManager->InitTexture("TESR_AmbientOcclusionBuffer", &Textures.BufferTexture, &Textures.BufferSurface,
		TheRenderManager->width, TheRenderManager->height, D3DFMT_R16F);
	TheTextureManager->InitTexture("TESR_AmbientOcclusionScratch", &Textures.ScratchTexture, &Textures.ScratchSurface,
		TheRenderManager->width, TheRenderManager->height, D3DFMT_R16F);
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

// Render the occlusion into the effect's own buffer, ahead of the shadow composite. Returns whether
// it did; when it did not, the effect renders into the frame after the composite as before.
//
// Only a shader with the Compute and Apply techniques can: a replacement AmbientOcclusion.fx
// written for the frame keeps the frame path.
bool AmbientOcclusionEffect::RenderBuffers(IDirect3DDevice9* Device) {
	if (!Enabled || !Effect || !ShouldRender() || !Textures.BufferSurface || !Textures.ScratchSurface)
		return false;

	D3DXTECHNIQUE_DESC compute, apply;
	D3DXHANDLE computeHandle = Effect->GetTechnique(1);
	D3DXHANDLE applyHandle = Effect->GetTechnique(2);
	if (!computeHandle || !applyHandle || FAILED(Effect->GetTechniqueDesc(computeHandle, &compute)) ||
		FAILED(Effect->GetTechniqueDesc(applyHandle, &apply)) || !compute.Name || !apply.Name ||
		strcmp(compute.Name, "Compute") || strcmp(apply.Name, "Apply"))
		return false;

	// Each pass draws into the buffer and is copied into the scratch, which the next pass reads.
	Device->SetRenderTarget(0, Textures.BufferSurface);
	Render(Device, Textures.BufferSurface, Textures.ScratchSurface, 1, false, NULL);
	return true;
}