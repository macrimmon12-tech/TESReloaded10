#include "VolumetricLight.h"

void VolumetricLightEffect::UpdateConstants() {}

void VolumetricLightEffect::UpdateSettings() {
	Settings.Strength = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "Strength");
	Settings.Anisotropy = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "Anisotropy");
	Settings.Dither = TheSettingManager->GetSettingI("Shaders.VolumetricLight.Main", "Dither");
	Settings.AccumDistance = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "AccumDistance");
	Settings.FogInfluence = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "FogInfluence");
	Settings.HeightFalloff = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "HeightFalloff");
	Settings.DitherMotion = TheSettingManager->GetSettingI("Shaders.VolumetricLight.Main", "DitherMotion");

	Settings.ScatterColor.x = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterR");
	Settings.ScatterColor.y = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterG");
	Settings.ScatterColor.z = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterB");

	Constants.Data1 = D3DXVECTOR4(Settings.ScatterColor.x, Settings.ScatterColor.y, Settings.ScatterColor.z, Settings.AccumDistance);
	// y is free -- extinction is no longer a setting of this effect. There is one atmosphere, so
	// the shader reads VolumetricFog's Extinction and converts it (see GROUND_LAYER_SCALE in
	// VolumetricLight.fx.hlsl), which also keeps the floor-above-zero that sigmaT's analytic
	// step integral needs in one place rather than two.
	Constants.Data3 = D3DXVECTOR4(Settings.Strength, 0.0f, Settings.FogInfluence, Settings.Anisotropy);
	// HeightFalloff passes through unclamped: 0 is a real setting, meaning a uniform medium with
	// no altitude gradient, and the shader tests for it explicitly.
	// x is free -- it carried the DebugView mode until the diagnostic views were removed.
	Constants.Data4 = D3DXVECTOR4(0.0f, Settings.Dither ? 1.0f : 0.0f,
		max(Settings.HeightFalloff, 0.0f), Settings.DitherMotion ? 1.0f : 0.0f);
}

void VolumetricLightEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData1", &Constants.Data1);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData3", &Constants.Data3);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData4", &Constants.Data4);
}

void VolumetricLightEffect::RegisterTextures() {
	// Half resolution: the march is the expensive part (14 cascade-shadow samples per pixel) and
	// the shaft has no fine detail that needs full res. Composite reads it through a bilinear
	// sampler, which upsamples it. Mirrors FlashlightBeamEffect's TESR_VolumetricBuffer.
	TheTextureManager->InitTexture("TESR_VolumetricLightBuffer", &Textures.VolumetricTexture, &Textures.VolumetricSurface,
		TheRenderManager->width / 2, TheRenderManager->height / 2, D3DFMT_A16B16G16R16F);
}

bool VolumetricLightEffect::ShouldRender() {
	return TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater;
}
