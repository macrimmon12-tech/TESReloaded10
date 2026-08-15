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

void LUTEffect::AssignLUTSlot(int slot, IDirect3DBaseTexture9* texture, const char* filename)
{
	if (!texture) return;

	const char* samplerName = nullptr;
	IDirect3DTexture9** member = nullptr;

	switch (slot) {
	case 0: samplerName = "TESR_LUTDayBuffer";      member = &DayTexture;      break;
	case 1: samplerName = "TESR_LUTNightBuffer";    member = &NightTexture;    break;
	case 2: samplerName = "TESR_LUTInteriorBuffer"; member = &InteriorTexture; break;
	default: return;
	}

	*member = (IDirect3DTexture9*)texture;
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
}

void LUTEffect::SaveLUTSetting(int slot, const char* filename)
{
	const char* key = (slot == 0) ? "DayLUT" : (slot == 1) ? "NightLUT" : (slot == 2) ? "InteriorLUT" : nullptr;
	if (!key) return;

	char buf[80];
	strncpy_s(buf, filename, sizeof(buf) - 1);
	TheSettingManager->SetSettingS("Shaders.LUT.Main", key, buf);
}

void LUTEffect::LoadLUT(int slot, const char* filename)
{
	if (!filename || filename[0] == '\0') return;

	std::string path = std::string(LUTFolder) + filename;
	IDirect3DBaseTexture9* tex = TheTextureManager->GetFileTexture(path, TextureRecord::PlanarBuffer);
	if (!tex) return;

	AssignLUTSlot(slot, tex, filename);
	SaveLUTSetting(slot, filename);
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

	// Restore previously-saved slot selections. These are already persisted —
	// load + assign only, no need to write them straight back out.
	auto restoreSlot = [this](int slot, const char* key) {
		char buf[80];
		TheSettingManager->GetSettingS("Shaders.LUT.Main", key, buf);
		if (!buf[0] || buf[0] == '"') return;

		std::string path = std::string(LUTFolder) + buf;
		IDirect3DBaseTexture9* tex = TheTextureManager->GetFileTexture(path, TextureRecord::PlanarBuffer);
		if (tex) AssignLUTSlot(slot, tex, buf);
	};

	restoreSlot(0, "DayLUT");
	restoreSlot(1, "NightLUT");
	restoreSlot(2, "InteriorLUT");
}

void LUTEffect::UpdateSettings()
{
	Settings.Strength        = TheSettingManager->GetSettingF("Shaders.LUT.Main", "Strength");
	Settings.PreTonemapping  = TheSettingManager->GetSettingI("Shaders.LUT.Main", "PreTonemapping");
	Settings.HDRCompat       = TheSettingManager->GetSettingI("Shaders.LUT.Main", "HDRCompat");
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
	Constants.Blend.z = Settings.HDRCompat ? 1.0f : 0.0f;
}
