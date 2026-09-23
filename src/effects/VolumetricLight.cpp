#include <cmath>

#include "VolumetricLight.h"

// Advance the march's dither each frame when something will average the frames back together.
//
// TAA is that something: its history blends roughly the last ten frames, so a dither that moves each
// frame averages out into smooth shafts, where a fixed one leaves the same grain in every frame and
// accumulation cannot touch it. So while TAA is enabled the dither moves on its own; DitherMotion
// forces it without TAA, where it only trades a still pattern for shimmer.
//
// The step is the golden ratio per frame, the additive sequence that spreads any run of frames most
// evenly over [0, 1). Counted in frames rather than seconds, so it is the same at any frame rate.
// Kept in double and wrapped here: the shader only ever sees the fractional offset.
void VolumetricLightEffect::UpdateConstants() {
	bool temporal = TheShaderManager->Effects.TAA && TheShaderManager->Effects.TAA->Enabled;
	if (Settings.DitherMotion || temporal) {
		ditherPhase = std::fmod(ditherPhase + 0.6180339887498949, 1.0);
		Constants.Data4.w = (float)ditherPhase;
	}
	else {
		Constants.Data4.w = 0.0f;
	}
}

void VolumetricLightEffect::UpdateSettings() {
	Settings.Strength = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "Strength");
	Settings.Anisotropy = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "Anisotropy");
	Settings.Dither = TheSettingManager->GetSettingI("Shaders.VolumetricLight.Main", "Dither");
	Settings.AccumDistance = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "AccumDistance");
	Settings.ScatterReference = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "ScatterReference");
	Settings.FogInfluence = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "FogInfluence");
	Settings.Extinction = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "Extinction");
	Settings.HeightFalloff = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Main", "HeightFalloff");
	Settings.DitherMotion = TheSettingManager->GetSettingI("Shaders.VolumetricLight.Main", "DitherMotion");

	Settings.ScatterColor.x = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterR");
	Settings.ScatterColor.y = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterG");
	Settings.ScatterColor.z = TheSettingManager->GetSettingF("Shaders.VolumetricLight.Coloring", "ScatterB");

	Constants.Data1 = D3DXVECTOR4(Settings.ScatterColor.x, Settings.ScatterColor.y, Settings.ScatterColor.z, Settings.AccumDistance);
	// Extinction floored above zero rather than at it: the shader divides by sigmaT to integrate
	// each step analytically. It carries its own epsilon for that, but keeping a real value here
	// means the medium always has some attenuation, which is what makes transmittance meaningful.
	Constants.Data3 = D3DXVECTOR4(Settings.Strength, max(Settings.Extinction, 0.001f), Settings.FogInfluence, Settings.Anisotropy);
	// HeightFalloff passes through unclamped: 0 is a real setting, meaning a uniform medium with
	// no altitude gradient, and the shader tests for it explicitly.
	//
	// ScatterReference is floored hard because the shader divides by it. It is the path length
	// the scattering and extinction coefficients are expressed against, and it is deliberately
	// NOT AccumDistance any more -- see the note at invReference in VolumetricLight.fx.hlsl.
	// w, the dither's per-frame offset, is set every frame in UpdateConstants.
	Constants.Data4 = D3DXVECTOR4(max(Settings.ScatterReference, 1.0f), Settings.Dither ? 1.0f : 0.0f,
		max(Settings.HeightFalloff, 0.0f), Constants.Data4.w);
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
