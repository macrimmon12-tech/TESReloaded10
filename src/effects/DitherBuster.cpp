#include "DitherBuster.h"

void DitherBusterEffect::UpdateConstants() {
	Constants.Data.x = Settings.Strength;
	Constants.Data.y = Settings.MaskPower;
	Constants.Data.z = Settings.DitherThreshold;
}

void DitherBusterEffect::UpdateSettings() {
	Settings.Strength  = TheSettingManager->GetSettingF("Shaders.DitherBuster.Main", "Strength");
	Settings.MaskPower = TheSettingManager->GetSettingF("Shaders.DitherBuster.Main", "MaskPower");
	Settings.DitherThreshold = TheSettingManager->GetSettingF("Shaders.DitherBuster.Main", "DitherThreshold");
}

void DitherBusterEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_DitherBusterData", &Constants.Data);
}
