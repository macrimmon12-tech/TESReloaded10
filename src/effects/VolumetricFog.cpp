#include "VolumetricFog.h"

void VolumetricFogEffect::UpdateConstants() {
	// live weather-driven sky-filter disable, smoothly animated the same way RainEffect
	// animates rain onset/offset (Animator started once on the true/false edge, sampled every frame).
	// Animator's clock runs in GAME HOURS, not real seconds (Animator.cpp derives currenttime from
	// GameDaysPassed) -- 0.1f here is a few game-minutes, a few real seconds at typical time scale,
	// matching the same order of magnitude RainEffect uses (0.05f/0.07f) rather than the ~6-real-minute
	// transition an unadjusted "3.0f meaning 3 seconds" mistake would have produced.
	bool shouldDisableFilter = (TheShaderManager->GameState.isRainy && rainyDisablesSkyFilter) ||
	                           (TheShaderManager->GameState.isCloudy && cloudyDisablesSkyFilter);

	if (shouldDisableFilter && weatherFilterWasActive) {
		// weather just started hiding the sky filter / disabling NVR-driven density
		weatherFilterWasActive = false;
		Constants.WeatherFilterAnimator.Start(0.1f, 0.0f);
	}
	else if (!shouldDisableFilter && !weatherFilterWasActive) {
		// weather cleared, sky filter/density fade back in
		weatherFilterWasActive = true;
		Constants.WeatherFilterAnimator.Start(0.1f, 1.0f);
	}

	Constants.Weather.x = Constants.WeatherFilterAnimator.GetValue();
	Constants.Weather.y = TheShaderManager->GameState.isExterior ? 1.0f : 0.0f;
	// SkyAmbientRadiance's TESR_SkyIrradiance is only refreshed while Shaders.Sky is enabled;
	// fall back to flat sky color in the shader when it's off rather than reading stale/zero data.
	Constants.Weather.z = TheShaderManager->Shaders.Sky->Enabled ? 1.0f : 0.0f;

	// Moon-phase-driven night ambient ceiling, mirroring ShadowsExterior.cpp's own moon-phase
	// shadow-fade formula (same day-count/phaseLength inputs, same cosine curve) so fog's night
	// floor tracks the same lunar cycle the shadow system already uses. The day/night fade itself
	// is left to the shader's own continuous isDayTime curve rather than duplicated here, so the
	// transition stays smooth instead of snapping at a hard cutoff. Harmless to compute for
	// interiors too -- NightAmbientStrength is zeroed there in UpdateSettings, so it has no effect.
	TimeGlobals* GameTimeGlobals = TimeGlobals::Get();
	float DaysPassed = GameTimeGlobals->GameDaysPassed ? GameTimeGlobals->GameDaysPassed->data : 1.0f;
	float MoonPhase = (fmod(DaysPassed, 8 * Tes->sky->firstClimate->phaseLength & 0x3F)) / (Tes->sky->firstClimate->phaseLength & 0x3F);
	MoonPhase = std::lerp(-D3DX_PI, D3DX_PI, MoonPhase / 8) - D3DX_PI / 4;
	Constants.Global.z = std::lerp(0.0f, nightMinDarkness, cosf(MoonPhase) * 0.5f + 0.5f);
}

void VolumetricFogEffect::UpdateSettings(){

	char SettingCategory[50] = "Shaders.VolumetricFog.";

	if (TheShaderManager->GameState.isExterior)
		strcat(SettingCategory, "Main");
	else
		strcat(SettingCategory, "Interiors");

	Constants.Global.x = TheSettingManager->GetSettingF(SettingCategory, "Amount");
	Constants.Weather.w = TheSettingManager->GetSettingF(SettingCategory, "FogSaturation");

	Constants.Density.x = TheSettingManager->GetSettingF(SettingCategory, "BaseDensity");
	Constants.Density.y = TheSettingManager->GetSettingF(SettingCategory, "WeatherImpact");

	Constants.Shape.x = TheSettingManager->GetSettingF(SettingCategory, "HeightFalloff");
	Constants.Shape.y = TheSettingManager->GetSettingF(SettingCategory, "MaxHeight");
	Constants.Shape.z = TheSettingManager->GetSettingF(SettingCategory, "Extinction");
	Constants.Shape.w = TheSettingManager->GetSettingF(SettingCategory, "Inscattering");

	float windAngleRad = TheSettingManager->GetSettingF(SettingCategory, "WindAngle") * 0.0174532925f; // degrees to radians
	Constants.Wind.x = cosf(windAngleRad);
	Constants.Wind.y = sinf(windAngleRad);
	Constants.Wind.z = TheSettingManager->GetSettingF(SettingCategory, "WindSpeed");
	Constants.Wind.w = TheSettingManager->GetSettingF(SettingCategory, "NoiseScale");

	Constants.Scatter.z = TheSettingManager->GetSettingF(SettingCategory, "NoiseStrength");
	Constants.Scatter.w = TheSettingManager->GetSettingF(SettingCategory, "HeightInfluence");

	rainyDisablesSkyFilter = TheSettingManager->GetSettingI(SettingCategory, "RainyDisablesSkyFilter") != 0;
	cloudyDisablesSkyFilter = TheSettingManager->GetSettingI(SettingCategory, "CloudyDisablesSkyFilter") != 0;

	if (TheShaderManager->GameState.isExterior) {
		Constants.Density.z = TheSettingManager->GetSettingF(SettingCategory, "MorningFogDip");
		Constants.Density.w = TheSettingManager->GetSettingF(SettingCategory, "SunriseSunsetBoost");

		Constants.Scatter.x = TheSettingManager->GetSettingF(SettingCategory, "PhaseAsymmetry");
		Constants.Scatter.y = TheSettingManager->GetSettingF(SettingCategory, "ShadowStrength");

		Constants.Aerial.x = TheSettingManager->GetSettingF(SettingCategory, "AerialStrength");
		Constants.Aerial.y = TheSettingManager->GetSettingF(SettingCategory, "AerialRangeStart");
		Constants.Aerial.z = TheSettingManager->GetSettingF(SettingCategory, "AerialTintBlend");
		Constants.Aerial.w = TheSettingManager->GetSettingF(SettingCategory, "AerialDayFadeStart");

		Constants.AerialTintColor.x = TheSettingManager->GetSettingF(SettingCategory, "AerialTintR");
		Constants.AerialTintColor.y = TheSettingManager->GetSettingF(SettingCategory, "AerialTintG");
		Constants.AerialTintColor.z = TheSettingManager->GetSettingF(SettingCategory, "AerialTintB");

		Constants.Distant.x = TheSettingManager->GetSettingF(SettingCategory, "DistantFogRange");
		Constants.Distant.y = TheSettingManager->GetSettingF(SettingCategory, "DistantFogBlend");
		Constants.Distant.z = TheSettingManager->GetSettingF(SettingCategory, "DistantFogHeight");
		Constants.Distant.w = TheSettingManager->GetSettingF(SettingCategory, "EdgeAA");

		Constants.Global.y = TheSettingManager->GetSettingF(SettingCategory, "NightAmbientStrength");
		// shared with ShadowsExterior's own moon-phase shadow fade -- keeps fog's night-ambient
		// ceiling consistent with the shadow system's, rather than a second, disconnected knob.
		nightMinDarkness = 1.0f - TheSettingManager->GetSettingF("Shaders.ShadowsExteriors.Main", "NightMinDarkness");

		// Volumetric light-shaft raymarch. Strength is read from Shaders.GodRays.Main's own Quality
		// setting (0: Classic, 1: Enhanced, 2: Volumetric) rather than a separate enable flag here --
		// that's the single master switch shared with GodRaysEffect::selectedTechnique, so the two
		// stay in sync without fog needing to know GodRays' internals beyond that one setting.
		int lightShaftsQuality = TheSettingManager->GetSettingI("Shaders.GodRays.Main", "Quality");
		Constants.Shaft.x = (lightShaftsQuality == 2) ? TheSettingManager->GetSettingF(SettingCategory, "VolumetricShaftStrength") : 0.0f;
		Constants.Shaft.y = TheSettingManager->GetSettingF(SettingCategory, "VolumetricShaftSteps");
		Constants.Shaft.z = TheSettingManager->GetSettingF(SettingCategory, "VolumetricShaftRange");
	}
	else {
		// these settings don't do anything in interiors: no sun, no horizon, no distant view
		Constants.Density.z = 0.0f;
		Constants.Density.w = 0.0f;

		Constants.Scatter.x = 0.0f;
		Constants.Scatter.y = 0.0f;

		Constants.Aerial = D3DXVECTOR4(0, 0, 0, 0);
		Constants.AerialTintColor = D3DXVECTOR4(0, 0, 0, 0);
		Constants.Distant = D3DXVECTOR4(0, 0, 0, 0);

		Constants.Global.y = 0.0f;
		Constants.Shaft = D3DXVECTOR4(0, 0, 0, 0);
	}
}

void VolumetricFogEffect::RegisterConstants(){
	TheShaderManager->RegisterConstant("TESR_VolumetricFogDensity", &Constants.Density);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogShape", &Constants.Shape);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogWind", &Constants.Wind);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogScatter", &Constants.Scatter);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogWeather", &Constants.Weather);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogAerial", &Constants.Aerial);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogAerialTint", &Constants.AerialTintColor);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogDistant", &Constants.Distant);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogGlobal", &Constants.Global);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogShaft", &Constants.Shaft);
}


bool VolumetricFogEffect::ShouldRender()
{
	return !TheShaderManager->GameState.isUnderwater;
};
