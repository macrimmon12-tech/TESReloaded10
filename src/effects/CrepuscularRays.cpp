#include "CrepuscularRays.h"

void CrepuscularRaysEffect::UpdateConstants() {
	// Nothing per-frame beyond what UpdateSettings already caches -- unlike GodRays' Classic,
	// this effect's own ShouldRender() already gates out night entirely (see below), so there's
	// no day/night transition to lerp here.
}

void CrepuscularRaysEffect::UpdateSettings() {
	// Tint.xyz is unused now (was a scatter color tint, dropped with the phase-function model
	// that read it -- see CrepuscularRays.fx.hlsl's header). Left unset (zeroed) rather than removed
	// from the struct, since the shader still declares TESR_VolumetricLightData1 as a float4 and
	// only its w component needs a real value.
	Constants.Tint.w = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "AccumDistance");

	// Data.y/z/w unused now (were sample count, fog influence and anisotropy of the dropped model).
	Constants.Data.x = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "Strength");

	Constants.Debug.x = TheSettingManager->GetSettingF("Shaders.CrepuscularRays.Main", "DebugMode");
	Constants.Debug.y = TheSettingManager->GetSettingI("Shaders.CrepuscularRays.Main", "DitherEnabled");
}

void CrepuscularRaysEffect::RegisterConstants() {
	// Names match CrepuscularRays.fx.hlsl's own global declarations (see CrepuscularRaysStruct's comment).
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData1", &Constants.Tint);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData3", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData4", &Constants.Debug);
}

void CrepuscularRaysEffect::RegisterTextures() {
	// Half resolution: the march is the expensive part (64 shadow-atlas samples per pixel) and
	// the shaft has no fine detail the depth-aware composite upsample can't recover. Texture name
	// matches CrepuscularRays.fx.hlsl's own TESR_VolumetricLightBuffer sampler declaration.
	TheTextureManager->InitTexture("TESR_VolumetricLightBuffer", &Textures.MarchTexture, &Textures.MarchSurface,
		TheRenderManager->width / 2, TheRenderManager->height / 2, D3DFMT_A16B16G16R16F);
}

bool CrepuscularRaysEffect::ShouldRender() {
	return TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater && TheShaderManager->GameState.dayLight > 0.5;
}
