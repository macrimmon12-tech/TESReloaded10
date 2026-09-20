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

	// ---- interior point-light hero selection (tier 2 light shafts) ----
	// Picked once per frame in C++, not per pixel: SM3 cannot dynamically index the 12
	// separately-named shadow cubemap samplers (TESR_ShadowCubeMapBuffer0..11 are distinct
	// uniforms, not an array), so the choice has to be made here and the winner rebound through
	// a pair of dedicated sampler slots the shader always reads from by fixed name.
	//
	// Scored by brightness-weighted attenuation at the camera position, not raw distance: a
	// bright light a bit farther away can matter more than a dim one very close, and this
	// reuses the same falloff shape GetPointLightAtten (Effects/Includes/Shadows.hlsl) applies
	// when actually shading with the winner, so the score and the eventual visual strength agree.
	if (!TheShaderManager->GameState.isExterior) {
		D3DXVECTOR4& camPos = TheRenderManager->CameraPosition;
		float scores[ShadowCubeMapsMax];
		int bestIndex = -1;
		float bestScore = 0.0f;

		for (int i = 0; i < ShadowCubeMapsMax; i++) {
			D3DXVECTOR4& lightPos = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowLightPosition[i];
			scores[i] = 0.0f;
			if (lightPos.w <= 0.0f) continue; // empty slot

			float dx = lightPos.x - camPos.x, dy = lightPos.y - camPos.y, dz = lightPos.z - camPos.z;
			float dist = sqrtf(dx * dx + dy * dy + dz * dz) / lightPos.w; // normalized by radius
			float s = min(1.0f, dist * dist);
			float atten = max(0.0f, ((1.0f - s) * (1.0f - s)) / (1.0f + 5.0f * s));

			D3DXVECTOR4& color = TheShaderManager->LightColor[i];
			float luminance = (color.x + color.y + color.z) * color.w / 3.0f;

			scores[i] = atten * luminance;
			if (scores[i] > bestScore) {
				bestScore = scores[i];
				bestIndex = i;
			}
		}

		if (bestIndex == -1) {
			// nothing qualifies this frame (LightPoints=0, or no lights nearby)
			currentHeroLightIndex = -1;
			previousHeroLightIndex = -1;
		}
		else if (bestIndex != currentHeroLightIndex) {
			bool isFirstPick = (currentHeroLightIndex == -1);
			float currentScore = isFirstPick ? 0.0f : scores[currentHeroLightIndex];
			// Hysteresis: a challenger has to clearly beat the incumbent, not just edge it out,
			// or walking the midpoint between two similar lights would flip the hero every frame.
			if (isFirstPick || bestScore > currentScore * (1.0f + heroHysteresisMargin)) {
				previousHeroLightIndex = isFirstPick ? bestIndex : currentHeroLightIndex;
				currentHeroLightIndex = bestIndex;
				if (isFirstPick)
					Constants.HeroCrossfadeAnimator.Initialize(1.0f); // nothing to fade from yet
				else
					Constants.HeroCrossfadeAnimator.Start(heroCrossfadeDuration, 1.0f);
			}
		}
	}
	else {
		currentHeroLightIndex = -1;
		previousHeroLightIndex = -1;
	}

	if (currentHeroLightIndex >= 0) {
		HeroCubeMapA = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowCubeMapTexture[currentHeroLightIndex];
		Constants.HeroLightA = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowLightPosition[currentHeroLightIndex];
		Constants.HeroColorA = TheShaderManager->LightColor[currentHeroLightIndex];
	}
	else {
		HeroCubeMapA = nullptr;
		Constants.HeroLightA = D3DXVECTOR4(0, 0, 0, 0);
		Constants.HeroColorA = D3DXVECTOR4(0, 0, 0, 0);
	}

	if (previousHeroLightIndex >= 0) {
		HeroCubeMapB = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowCubeMapTexture[previousHeroLightIndex];
		Constants.HeroLightB = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowLightPosition[previousHeroLightIndex];
		Constants.HeroColorB = TheShaderManager->LightColor[previousHeroLightIndex];
	}
	else {
		HeroCubeMapB = nullptr;
		Constants.HeroLightB = D3DXVECTOR4(0, 0, 0, 0);
		Constants.HeroColorB = D3DXVECTOR4(0, 0, 0, 0);
	}

	// EffectRecord::SetCT only calls BindTexture when its cached pointer is null, so reassigning
	// HeroCubeMapA/B above does nothing on its own -- force a re-resolve every frame, or the
	// shader would keep sampling whichever light happened to bind first and never see a switch.
	ClearSampler("TESR_FogHeroCubeMapA", strlen("TESR_FogHeroCubeMapA"));
	ClearSampler("TESR_FogHeroCubeMapB", strlen("TESR_FogHeroCubeMapB"));

	Constants.HeroBlend.x = Constants.HeroCrossfadeAnimator.GetValue();
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

		Constants.AerialTintColor.x = TheSettingManager->GetSettingF(SettingCategory, "AerialTintR");
		Constants.AerialTintColor.y = TheSettingManager->GetSettingF(SettingCategory, "AerialTintG");
		Constants.AerialTintColor.z = TheSettingManager->GetSettingF(SettingCategory, "AerialTintB");

		Constants.Distant.x = TheSettingManager->GetSettingF(SettingCategory, "DistantFogRange");
		Constants.Distant.y = TheSettingManager->GetSettingF(SettingCategory, "DistantFogBlend");
		Constants.Distant.z = TheSettingManager->GetSettingF(SettingCategory, "DistantFogHeight");
		Constants.Distant.w = TheSettingManager->GetSettingF(SettingCategory, "EdgeAA");

		// point-light hero shafts are interior-only; nothing to tune outdoors
		Constants.HeroBlend.y = 0.0f;
		Constants.HeroBlend.z = 0.0f;
		heroHysteresisMargin = 0.25f;
		heroCrossfadeDuration = 0.05f;
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

		Constants.HeroBlend.y = TheSettingManager->GetSettingF(SettingCategory, "InteriorGlowStrength");
		Constants.HeroBlend.z = TheSettingManager->GetSettingF(SettingCategory, "InteriorShaftStrength");
		// Animator units are GAME HOURS, not seconds -- 0.05f is a few real seconds at typical
		// time scale, matching WeatherFilterAnimator's own 0.1f order of magnitude.
		heroCrossfadeDuration = TheSettingManager->GetSettingF(SettingCategory, "HeroCrossfadeDuration");
		heroHysteresisMargin = TheSettingManager->GetSettingF(SettingCategory, "HeroHysteresisMargin");
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
	TheShaderManager->RegisterConstant("TESR_VolumetricFogHeroLightA", &Constants.HeroLightA);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogHeroLightB", &Constants.HeroLightB);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogHeroColorA", &Constants.HeroColorA);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogHeroColorB", &Constants.HeroColorB);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogHeroBlend", &Constants.HeroBlend);
}

void VolumetricFogEffect::RegisterTextures() {
	// Placeholders, initially null -- UpdateConstants reassigns what these point at every frame
	// to whichever light currently wins the hero selection, and forces a re-resolve via
	// ClearSampler since the registration below only wires up the NAME once.
	TheTextureManager->RegisterTexture("TESR_FogHeroCubeMapA", (IDirect3DBaseTexture9**)&HeroCubeMapA);
	TheTextureManager->RegisterTexture("TESR_FogHeroCubeMapB", (IDirect3DBaseTexture9**)&HeroCubeMapB);
}


bool VolumetricFogEffect::ShouldRender()
{
	return !TheShaderManager->GameState.isUnderwater;
};
