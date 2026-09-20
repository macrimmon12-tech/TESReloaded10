#include "VolumetricLight.h"

void VolumetricLightEffect::UpdateConstants() {}

void VolumetricLightEffect::UpdateSettings() {
	Settings.Strength = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "Strength");
	Settings.FogDensity = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "FogDensity");
	Settings.Height = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "Height");
	Settings.Anisotropy = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "Anisotropy");
	Settings.SkyScatterStrength = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "SkyScatterStrength");
	Settings.Dither = TheSettingManager->GetSettingI("Shaders.VolumetricLight.Main", "Dither");
	Settings.AccumDistance = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "AccumDistance");

	Settings.ScatterColor.x = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterR");
	Settings.ScatterColor.y = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterG");
	Settings.ScatterColor.z = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterB");

	Settings.WindDirection.x = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Wind", "DirectionX");
	Settings.WindDirection.y = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Wind", "DirectionY");
	Settings.WindDirection.z = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Wind", "DirectionZ");

	Constants.Data1 = D3DXVECTOR4(Settings.ScatterColor.x, Settings.ScatterColor.y, Settings.ScatterColor.z, Settings.AccumDistance);
	Constants.Data2 = D3DXVECTOR4(Settings.WindDirection.x, Settings.WindDirection.y, Settings.WindDirection.z, 0.0f);
	Constants.Data3 = D3DXVECTOR4(Settings.Strength, Settings.FogDensity, Settings.Height, Settings.Anisotropy);
	Constants.Data4 = D3DXVECTOR4(Settings.SkyScatterStrength, Settings.Dither ? 1.0f : 0.0f, 0.0f, 0.0f);
}

void VolumetricLightEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData1", &Constants.Data1);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData2", &Constants.Data2);
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
