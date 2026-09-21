#include "GodRays.h"

void GodRaysEffect::UpdateConstants() {
	Constants.Data.z = std::lerp(nightMult, dayMult, TheShaderManager->GameState.transitionCurve);
	Constants.Ray.w = rayVisibility * sunGlareEnabled ? TheShaderManager->ShaderConst.sunGlare : 1.0;

	// Volumetric/Enhanced's corona strength: the weather's own authored sun-glare value when
	// SunGlareEnabled is on, full strength otherwise. Deliberately not reusing the Ray.w line
	// above for this -- Ray.w is unused by Classic's own HLSL (see TESR_GodRaysRay's comment) and
	// its `rayVisibility * sunGlareEnabled ? x : y` has an operator-precedence bug (evaluates as
	// `(rayVisibility * sunGlareEnabled) ? x : y`, silently discarding rayVisibility as a scalar)
	// that's harmless there only because nothing reads it; left untouched since Classic stays
	// byte-for-byte unchanged, not carried into this new, actually-live computation.
	Constants.Enhanced.w = sunGlareEnabled ? TheShaderManager->ShaderConst.sunGlare : 1.0f;
}

void GodRaysEffect::UpdateSettings(){
	dayMult = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "DayMultiplier");
	nightMult = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "NightMultiplier");
	sunGlareEnabled = TheSettingManager->GetSettingI("Shaders.GodRays.Main", "SunGlareEnabled");
	// 0: Classic, 1: Enhanced, 2: Volumetric.
	selectedTechnique = TheSettingManager->GetSettingI("Shaders.GodRays.Main", "Quality");

	Constants.Ray.x = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "RayIntensity");
	Constants.Ray.y = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "RayLength");
	Constants.Ray.z = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "RayDensity");
	rayVisibility = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "RayVisibility");
	Constants.RayColor.x = TheSettingManager->GetSettingF("Shaders.GodRays.Coloring", "RayR");
	Constants.RayColor.y = TheSettingManager->GetSettingF("Shaders.GodRays.Coloring", "RayG");
	Constants.RayColor.z = TheSettingManager->GetSettingF("Shaders.GodRays.Coloring", "RayB");
	Constants.RayColor.w = TheSettingManager->GetSettingF("Shaders.GodRays.Coloring", "Saturate");
	Constants.Data.x = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "LightShaftPasses");
	Constants.Data.y = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "Luminance");
	Constants.Data.w = TheSettingManager->GetSettingF("Shaders.GodRays.Main", "TimeEnabled");

	Constants.Enhanced.x = TheSettingManager->GetSettingF("Shaders.GodRays.Enhanced", "RayDecay");
	Constants.Enhanced.y = TheSettingManager->GetSettingF("Shaders.GodRays.Enhanced", "RayStepScale");
	Constants.Enhanced.z = TheSettingManager->GetSettingF("Shaders.GodRays.Enhanced", "BlurStrength");

	Constants.Volumetric1.x = TheSettingManager->GetSettingF("Shaders.GodRays.Volumetric", "Steps");
	Constants.Volumetric1.y = TheSettingManager->GetSettingF("Shaders.GodRays.Volumetric", "MaxDistance");
	Constants.Volumetric1.z = TheSettingManager->GetSettingF("Shaders.GodRays.Volumetric", "HeightCutoff");
	Constants.Volumetric1.w = TheSettingManager->GetSettingF("Shaders.GodRays.Volumetric", "LayerThickness");
	Constants.Volumetric2.x = TheSettingManager->GetSettingF("Shaders.GodRays.Volumetric", "ShadowedCutoffDistance");
	Constants.Volumetric2.y = TheSettingManager->GetSettingF("Shaders.GodRays.Volumetric", "NearWeightFalloff");
	Constants.Volumetric2.z = TheSettingManager->GetSettingF("Shaders.GodRays.Volumetric", "Strength");
}

void GodRaysEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_GodRaysRay", &Constants.Ray);
	TheShaderManager->RegisterConstant("TESR_GodRaysRayColor", &Constants.RayColor);
	TheShaderManager->RegisterConstant("TESR_GodRaysData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_GodRaysEnhanced", &Constants.Enhanced);
	TheShaderManager->RegisterConstant("TESR_GodRaysVolumetric1", &Constants.Volumetric1);
	TheShaderManager->RegisterConstant("TESR_GodRaysVolumetric2", &Constants.Volumetric2);
}

bool GodRaysEffect::ShouldRender() {
	return TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater && TheShaderManager->GameState.dayLight > 0.5;
}