#include "FlashlightBeam.h"

void FlashlightBeamEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_VolumetricControl", &Constants.Control);
}

void FlashlightBeamEffect::RegisterTextures() {
	// Half resolution: the march is the expensive part and the shaft has no fine detail.
	// The Flashlight Combine pass reads it through a bilinear sampler, which upsamples it.
	TheTextureManager->InitTexture("TESR_VolumetricBuffer", &Textures.VolumetricTexture, &Textures.VolumetricSurface,
		TheRenderManager->width / 2, TheRenderManager->height / 2, D3DFMT_A16B16G16R16F);
}

void FlashlightBeamEffect::UpdateSettings() {
	// UpdateConstants only runs while this effect is enabled, so switching it off would
	// otherwise leave the last gate value standing and the Flashlight Combine pass would
	// keep compositing a stale buffer. UpdateSettings runs on every settings change
	// regardless of enabled state, and before UpdateConstants, so clearing it here both
	// closes the gate on disable and leaves the enabled path free to set it again.
	Constants.Control = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);

	Settings.FirstPerson = TheSettingManager->GetSettingI("Shaders.FlashlightBeam.Main", "FirstPerson");
	Settings.ThirdPerson = TheSettingManager->GetSettingI("Shaders.FlashlightBeam.Main", "ThirdPerson");
	Settings.Strength = TheSettingManager->GetSettingF("Shaders.FlashlightBeam.Main", "Strength");
	Settings.Anisotropy = TheSettingManager->GetSettingF("Shaders.FlashlightBeam.Main", "Anisotropy");
	Settings.HeightFalloff = TheSettingManager->GetSettingF("Shaders.FlashlightBeam.Main", "HeightFalloff");
	Settings.MaxDistance = TheSettingManager->GetSettingF("Shaders.FlashlightBeam.Main", "MaxDistance");
	Settings.FirstPersonStrength = TheSettingManager->GetSettingF("Shaders.FlashlightBeam.Main", "FirstPersonStrength");
	Settings.FirstPersonOffsetRight = TheSettingManager->GetSettingF("Shaders.FlashlightBeam.Main", "FirstPersonOffsetRight");
	Settings.FirstPersonOffsetDown = TheSettingManager->GetSettingF("Shaders.FlashlightBeam.Main", "FirstPersonOffsetDown");
}

void FlashlightBeamEffect::UpdateConstants() {
	// Strength folds the effect toggle, the per view toggle and the configured strength into
	// one value, so a single > 0 test gates both the march and the Combine composite. That
	// matters because the buffer persists between frames: without the gate on the composite
	// side, the last shaft rendered would keep bleeding in after the beam is switched off.
	bool viewEnabled = (Player && Player->isThirdPerson) ? Settings.ThirdPerson : Settings.FirstPerson;
	bool lightOn = TheShaderManager->Effects.Flashlight->Enabled && TheShaderManager->Effects.Flashlight->spotLightActive;
	float strength = (Enabled && viewEnabled && lightOn) ? Settings.Strength : 0.0f;

	Constants.Control = D3DXVECTOR4(strength, Settings.Anisotropy, Settings.HeightFalloff, Settings.MaxDistance);
}
