#include "FXAA.h"

bool FXAAEffect::ShouldRender() {
	return TheSettingManager->SettingsMain.Main.FXAA;
}
