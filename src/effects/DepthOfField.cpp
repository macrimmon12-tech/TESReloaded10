#include "DepthOfField.h"

void DepthOfFieldEffect::UpdateConstants() {
	TheShaderManager->avglumaRequired = true;

	ValuesStruct* category = &Settings.FirstPerson;
	if (TheCameraManager->IsVanity())
		category = &Settings.VanityView;
	else if (!TheCameraManager->IsFirstPerson())
		category = &Settings.ThirdPerson;

	int Mode = category->Mode;
	int dofActive = category->Enabled;

	if ((Mode == 1 && (TheShaderManager->GameState.isDialog || TheShaderManager->GameState.isPersuasion)) ||
		(Mode == 2 && (!TheShaderManager->GameState.isDialog)) ||
		(Mode == 3 && (!TheShaderManager->GameState.isPersuasion)) ||
		(Mode == 4 && (!TheShaderManager->GameState.isDialog || !TheShaderManager->GameState.isPersuasion)))
		dofActive = 0;

	Constants.Enabled = dofActive;

	Constants.Blur.x = category->DistantBlur;
	Constants.Blur.y = category->DistantBlurStartRange;
	Constants.Blur.z = category->DistantBlurEndRange;
	Constants.Blur.w = category->BlurRadius;

	Constants.Data.x = category->FocusRange * dofActive;
	Constants.Data.y = 0.0f;
	Constants.Data.z = 0.0f;
	Constants.Data.w = category->NearBlurCutOff;

	Constants.MatsoData.x = category->BokehCurve;
	Constants.MatsoData.y = (float)category->SampleCount;
	Constants.MatsoData.z = category->ChromaticAberration;
	Constants.MatsoData.w = category->AnamorphicRatio;
}

void DepthOfFieldEffect::UpdateSettings() {
	auto loadView = [&](ValuesStruct& v, const char* section) {
		v.DistantBlur           = TheSettingManager->GetSettingF(section, "DistantBlur");
		v.DistantBlurStartRange = TheSettingManager->GetSettingF(section, "DistantBlurStartRange");
		v.DistantBlurEndRange   = TheSettingManager->GetSettingF(section, "DistantBlurEndRange");
		v.BlurRadius            = TheSettingManager->GetSettingF(section, "BlurRadius");
		v.FocusRange            = TheSettingManager->GetSettingF(section, "FocusRange");
		v.NearBlurCutOff        = TheSettingManager->GetSettingF(section, "NearBlurCutOff");
		v.BokehCurve            = TheSettingManager->GetSettingF(section, "BokehCurve");
		v.SampleCount           = TheSettingManager->GetSettingI(section, "SampleCount");
		v.ChromaticAberration   = TheSettingManager->GetSettingF(section, "ChromaticAberration");
		v.AnamorphicRatio       = TheSettingManager->GetSettingF(section, "AnamorphicRatio");
		v.Mode                  = TheSettingManager->GetSettingI(section, "Mode");
		v.Enabled               = TheSettingManager->GetSettingI(section, "Enabled");
	};

	loadView(Settings.FirstPerson,  "Shaders.DepthOfField.FirstPersonView");
	loadView(Settings.ThirdPerson,  "Shaders.DepthOfField.ThirdPersonView");
	loadView(Settings.VanityView,   "Shaders.DepthOfField.VanityView");
}

void DepthOfFieldEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_DepthOfFieldBlur",  &Constants.Blur);
	TheShaderManager->RegisterConstant("TESR_DepthOfFieldData",  &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_DOFMatsoData",      &Constants.MatsoData);
}

bool DepthOfFieldEffect::ShouldRender() {
	return Constants.Enabled || Constants.Blur.x;
}
