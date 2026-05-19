#include "VolumetricFog.h"

void VolumetricFogEffect::UpdateConstants() {
	// Wind/drift scroll vectors (direction * speed; shader multiplies by timetick)
	Sky* sky = Tes->sky;
	if (sky && TheShaderManager->GameState.isExterior) {
		float windSpd = sky->windSpeed;
		float windDir = Constants.UseWindDirection ? sky->windDirection : Constants.WindFallbackDir;
		Constants.Wind.x = windSpd * Constants.WindBaseSpeed * cosf(windDir);
		Constants.Wind.y = windSpd * Constants.WindBaseSpeed * sinf(windDir);
	} else {
		Constants.Wind.x = 0.0f;
		Constants.Wind.y = 0.0f;
	}
	Constants.Wind.z = Constants.InteriorDriftSpeed * cosf(Constants.InteriorDriftDir);
	Constants.Wind.w = Constants.InteriorDriftSpeed * sinf(Constants.InteriorDriftDir);

	// Select top 2 lights by brightness/distance score across both light pools
	const D3DXVECTOR4& camPos = TheRenderManager->CameraPosition;
	float topScore[2] = { -1.0f, -1.0f };
	Constants.FogLight[0] = Constants.FogLight[1] = D3DXVECTOR4(0, 0, 0, 1);
	Constants.FogLightColor[0] = Constants.FogLightColor[1] = D3DXVECTOR4(0, 0, 0, 0);

	auto evalLight = [&](const D3DXVECTOR4& pos, const D3DXVECTOR4& col) {
		if (pos.w < 1.0f) return; // empty or zero-radius slot
		float dx = pos.x - camPos.x, dy = pos.y - camPos.y, dz = pos.z - camPos.z;
		float dist2 = dx*dx + dy*dy + dz*dz;
		float luma = col.x * 0.2126f + col.y * 0.7152f + col.z * 0.0722f;
		float score = luma * col.w / (1.0f + dist2 / (pos.w * pos.w));
		if (score > topScore[0]) {
			topScore[1] = topScore[0]; Constants.FogLight[1] = Constants.FogLight[0]; Constants.FogLightColor[1] = Constants.FogLightColor[0];
			topScore[0] = score;       Constants.FogLight[0] = pos;                   Constants.FogLightColor[0] = col;
		} else if (score > topScore[1]) {
			topScore[1] = score; Constants.FogLight[1] = pos; Constants.FogLightColor[1] = col;
		}
	};

	// Shadow-casting lights (positions in ShadowsExteriors, colors in LightColor[0..ShadowCubeMapsMax-1])
	auto& shadowPos = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowLightPosition;
	for (int i = 0; i < ShadowCubeMapsMax; i++)
		evalLight(shadowPos[i], TheShaderManager->LightColor[i]);

	// Regular tracked lights (LightPosition[i], LightColor[ShadowCubeMapsMax+i])
	for (int i = 0; i < TrackedLightsMax; i++)
		evalLight(TheShaderManager->LightPosition[i], TheShaderManager->LightColor[ShadowCubeMapsMax + i]);
}

void VolumetricFogEffect::UpdateSettings(){

	char SettingCategory[50] = "Shaders.VolumetricFog.";
	
	if (TheShaderManager->GameState.isExterior) 
		strcat(SettingCategory, "Main");
	else
		strcat(SettingCategory, "Interiors");

	Constants.Data.x = TheSettingManager->GetSettingF(SettingCategory, "MinimumBaseFog");
	//Constants.Data.y = TheSettingManager->GetSettingF(SettingCategory, "ColorCoeff");
	//Constants.Data.w = TheSettingManager->GetSettingF(SettingCategory, "MaxDistance");
	Constants.Data.z = TheSettingManager->GetSettingF(SettingCategory, "Amount");

	Constants.LowFog.x = TheSettingManager->GetSettingF(SettingCategory, "FogSaturation");
	Constants.LowFog.y = TheSettingManager->GetSettingF(SettingCategory, "WeatherImpact");

	Constants.HighFog.x = TheSettingManager->GetSettingF(SettingCategory, "HeightFogDensity");
	Constants.HighFog.y = TheSettingManager->GetSettingF(SettingCategory, "HeightFogFalloff");
	Constants.HighFog.z = TheSettingManager->GetSettingF(SettingCategory, "HeightFogDist");

	Constants.Height.y = TheSettingManager->GetSettingF(SettingCategory, "HeightFogHeight");
	Constants.Height.z = TheSettingManager->GetSettingF(SettingCategory, "SimpleFogHeight");
	Constants.Height.w = TheShaderManager->GameState.isExterior ? 1.0f : 0.0f;

	Constants.SimpleFog.x = TheSettingManager->GetSettingF(SettingCategory, "Extinction");
	Constants.SimpleFog.y = TheSettingManager->GetSettingF(SettingCategory, "Inscattering");

	Constants.Blend.y = TheSettingManager->GetSettingF(SettingCategory, "HeightFogBlend");
	Constants.Blend.z = TheSettingManager->GetSettingF(SettingCategory, "HeightFogRolloff");
	Constants.Blend.w = TheSettingManager->GetSettingF(SettingCategory, "SimpleFogBlend");

	Constants.Noise.x = TheSettingManager->GetSettingF(SettingCategory, "NoiseFrequency");
	Constants.Noise.y = TheSettingManager->GetSettingF(SettingCategory, "NoiseStrength");

	if (TheShaderManager->GameState.isExterior) {
		Constants.Height.x = TheSettingManager->GetSettingF(SettingCategory, "DistantFogHeight");
		Constants.LowFog.z = TheSettingManager->GetSettingF(SettingCategory, "DistantFogRange");
		Constants.Blend.x = TheSettingManager->GetSettingF(SettingCategory, "DistantFogBlend");

		Constants.LowFog.w = TheSettingManager->GetSettingF(SettingCategory, "SunPower");
		Constants.SimpleFog.z = TheSettingManager->GetSettingF(SettingCategory, "FogNight");
		Constants.SimpleFog.w = TheSettingManager->GetSettingF(SettingCategory, "SimpleFogSkyColor");
		Constants.HighFog.w = TheSettingManager->GetSettingF(SettingCategory, "HeightFogSkyColor");

		Constants.WindBaseSpeed    = TheSettingManager->GetSettingF(SettingCategory, "WindSpeed");
		Constants.UseWindDirection = TheSettingManager->GetSettingI(SettingCategory, "UseWindDirection") != 0;
		Constants.WindFallbackDir  = TheSettingManager->GetSettingF(SettingCategory, "WindDirection");
		Constants.Noise.z = TheSettingManager->GetSettingF(SettingCategory, "SunAttenuation");
		Constants.Noise.w = TheSettingManager->GetSettingF(SettingCategory, "FogLightStrength");
	}
	else {
		// these settings don't do anything in interiors
		Constants.Height.x = 0.0;
		Constants.LowFog.z = 0.0;
		Constants.Blend.x = 0.0;

		Constants.LowFog.w = 0.0;
		Constants.SimpleFog.z = 1.0;
		Constants.SimpleFog.w = 0.0;
		Constants.HighFog.w = 0.0;

		Constants.InteriorDriftSpeed = TheSettingManager->GetSettingF(SettingCategory, "DriftSpeed");
		Constants.InteriorDriftDir   = TheSettingManager->GetSettingF(SettingCategory, "DriftDirection");
		Constants.Noise.z = 0.0f; // no sun indoors
		Constants.Noise.w = TheSettingManager->GetSettingF(SettingCategory, "FogLightStrength");
	}


}

void VolumetricFogEffect::RegisterConstants(){
	TheShaderManager->RegisterConstant("TESR_VolumetricFogLow", &Constants.LowFog);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogHigh", &Constants.HighFog);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogSimple", &Constants.SimpleFog);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogBlend", &Constants.Blend);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogHeight", &Constants.Height);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogWind", &Constants.Wind);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogNoise", &Constants.Noise);
	TheShaderManager->RegisterConstant("TESR_FogLight0", &Constants.FogLight[0]);
	TheShaderManager->RegisterConstant("TESR_FogLight1", &Constants.FogLight[1]);
	TheShaderManager->RegisterConstant("TESR_FogLightColor0", &Constants.FogLightColor[0]);
	TheShaderManager->RegisterConstant("TESR_FogLightColor1", &Constants.FogLightColor[1]);
}


bool VolumetricFogEffect::ShouldRender() 
{
	return !TheShaderManager->GameState.isUnderwater;
};