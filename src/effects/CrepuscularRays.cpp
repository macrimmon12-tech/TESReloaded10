#include "CrepuscularRays.h"

void CrepuscularRaysEffect::UpdateConstants() {
	// Nothing per-frame beyond what UpdateSettings already caches -- unlike GodRays' Classic,
	// this effect's own ShouldRender() already gates out night entirely (see below), so there's
	// no day/night transition to lerp here.
}

void CrepuscularRaysEffect::UpdateSettings() {
	Constants.Tint.x = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "TintR");
	Constants.Tint.y = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "TintG");
	Constants.Tint.z = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "TintB");
	Constants.Tint.w = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "AccumDistance");

	Constants.Data.x = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "Strength");
	Constants.Data.z = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "FogInfluence");
	Constants.Data.w = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "Anisotropy");

	Constants.Debug.x = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "DebugMode");
	Constants.Debug.y = TheSettingManager->GetSettingI("Shaders.CrepuscularRays.Main", "DitherEnabled");
}

void CrepuscularRaysEffect::RegisterConstants() {
	// Names match CrepuscularRays.fx.hlsl verbatim (see CrepuscularRaysStruct's own comment).
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData1", &Constants.Tint);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData3", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData4", &Constants.Debug);
}

void CrepuscularRaysEffect::RegisterTextures() {
	// Half resolution: the march is the expensive part (64 shadow-atlas samples per pixel) and
	// the shaft has no fine detail the depth-aware composite upsample can't recover. Texture name
	// matches CrepuscularRays.fx.hlsl's own TESR_VolumetricLightBuffer sampler declaration verbatim.
	TheTextureManager->InitTexture("TESR_VolumetricLightBuffer", &Textures.MarchTexture, &Textures.MarchSurface,
		TheRenderManager->width / 2, TheRenderManager->height / 2, D3DFMT_A16B16G16R16F);
}

bool CrepuscularRaysEffect::ShouldRender() {
	return TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater && TheShaderManager->GameState.dayLight > 0.5;
}
