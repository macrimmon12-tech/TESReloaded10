#include <filesystem>
#include "LUT.h"

const char* LUTEffect::LUTFolder = "Data/Textures/NewVegasReloaded/LUTs/";

void LUTEffect::ScanLUTFolder()
{
	LUTFiles.clear();
	namespace fs = std::filesystem;
	std::error_code ec;
	for (auto& entry : fs::directory_iterator(LUTFolder, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		std::string ext = entry.path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == ".png" || ext == ".dds" || ext == ".bmp")
			LUTFiles.push_back(entry.path().filename().string());
	}
	std::sort(LUTFiles.begin(), LUTFiles.end());
}

void LUTEffect::LoadLUT(int slot, const char* filename)
{
	if (!filename || filename[0] == '\0') return;

	std::string path = std::string(LUTFolder) + filename;
	IDirect3DBaseTexture9* tex = TheTextureManager->GetFileTexture(path, TextureRecord::PlanarBuffer);
	if (!tex) return;

	const char* samplerName = nullptr;
	IDirect3DTexture9** member = nullptr;

	switch (slot) {
	case 0: samplerName = "TESR_LUTDayBuffer";      member = &DayTexture;      break;
	case 1: samplerName = "TESR_LUTNightBuffer";    member = &NightTexture;    break;
	case 2: samplerName = "TESR_LUTInteriorBuffer"; member = &InteriorTexture; break;
	default: return;
	}

	*member = (IDirect3DTexture9*)tex;
	ClearSampler(samplerName, strlen(samplerName));

	if (slot == 0 && DayTexture) {
		D3DSURFACE_DESC desc;
		DayTexture->GetLevelDesc(0, &desc);
		DayCellCount = (float)desc.Height; // 16 for 256x16, 32 for 1024x32, 64 for 4096x64
	}

	// Find index in LUTFiles so cycle arrows stay in sync
	int& idx = (slot == 0) ? DayIdx : (slot == 1) ? NightIdx : InteriorIdx;
	for (int i = 0; i < (int)LUTFiles.size(); i++) {
		if (LUTFiles[i] == filename) { idx = i; break; }
	}

	// Persist chosen filename back to settings
	const char* key = (slot == 0) ? "DayLUT" : (slot == 1) ? "NightLUT" : "InteriorLUT";
	char buf[80];
	strncpy_s(buf, filename, sizeof(buf) - 1);
	TheSettingManager->SetSettingS("Shaders.LUT.Main", key, buf);
}

void LUTEffect::RegisterConstants()
{
	TheShaderManager->RegisterConstant("TESR_LUTData",  &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_LUTBlend", &Constants.Blend);
}

void LUTEffect::RegisterTextures()
{
	TheTextureManager->RegisterTexture("TESR_LUTDayBuffer",      (IDirect3DBaseTexture9**)&DayTexture);
	TheTextureManager->RegisterTexture("TESR_LUTNightBuffer",    (IDirect3DBaseTexture9**)&NightTexture);
	TheTextureManager->RegisterTexture("TESR_LUTInteriorBuffer", (IDirect3DBaseTexture9**)&InteriorTexture);

	ScanLUTFolder();

	char buf[80];
	TheSettingManager->GetSettingS("Shaders.LUT.Main", "DayLUT",      buf); if (buf[0] && buf[0] != '"') LoadLUT(0, buf);
	TheSettingManager->GetSettingS("Shaders.LUT.Main", "NightLUT",    buf); if (buf[0] && buf[0] != '"') LoadLUT(1, buf);
	TheSettingManager->GetSettingS("Shaders.LUT.Main", "InteriorLUT", buf); if (buf[0] && buf[0] != '"') LoadLUT(2, buf);
}

void LUTEffect::UpdateSettings()
{
	Settings.Strength = TheSettingManager->GetSettingF("Shaders.LUT.Main", "Strength");
}

void LUTEffect::UpdateConstants()
{
	Constants.Data.x = DayCellCount;
	Constants.Data.y = Settings.Strength;

	if (TheShaderManager->GameState.isExterior) {
		Constants.Blend.x = TheShaderManager->GameState.transitionCurve;
		Constants.Blend.y = 0.0f;
	} else {
		Constants.Blend.x = 0.0f;
		Constants.Blend.y = 1.0f;
	}
}
