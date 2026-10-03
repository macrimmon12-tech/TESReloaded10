#include "POM.h"

void POMShaders::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_ParallaxData", &Constants.Data);
}

void POMShaders::UpdateSettings() {
		Constants.Data.x = TheSettingManager->GetSettingF("Shaders.POM.Main", "HeightMapScale");
		Constants.Data.y = TheSettingManager->GetSettingF("Shaders.PBR.Status", "Enabled");
}

// .w: [Main.Main.ReducedQuality] ParallaxLite (Parallax.hlsl), read every frame so the switch applies at once.
void POMShaders::UpdateConstants() {
	Constants.Data.w = TheSettingManager->SettingsMain.Main.ParallaxLite ? 1.0f : 0.0f;
}
