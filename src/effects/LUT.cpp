#include <filesystem>
#include "LUT.h"
#include "LUTIdentity.h"
#include "LUTFile.h"

// LUT files go through LUTFile.h instead of the generic texture loader: images keep their exact size
// (no power-of-two resampling) and .cube files are read directly. Cached with the other file textures.
static IDirect3DBaseTexture9* GetLUTTexture(std::string path)
{
	if (IDirect3DBaseTexture9* cached = TheTextureManager->GetCachedTexture(path)) return cached;
	std::string error;
	IDirect3DTexture9* texture = LoadLUTTexture(TheRenderManager->device, path.c_str(), error);
	if (!texture) {
		Logger::Log("[ERROR] : Couldn't load LUT %s: %s", path.c_str(), error.c_str());
		return nullptr;
	}
	D3DSURFACE_DESC desc = {};
	texture->GetLevelDesc(0, &desc);
	Logger::Log("Loaded LUT %s (%ux%u)", path.c_str(), desc.Width, desc.Height);
	TheTextureManager->TextureCache[path] = texture;
	return texture;
}

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
		if (ext == ".png" || ext == ".dds" || ext == ".bmp" || ext == ".cube")
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

	// Each slot keeps its own cell count (16 for 256x16, 32 for 1024x32, 64 for 4096x64). The day LUT's
	// count used to be applied to all three, which scrambled a night or interior LUT of another size.
	// A texture that is not an N*N x N strip cannot be sampled as one, so it is not used at all.
	D3DSURFACE_DESC desc = {};
	const bool strip = SUCCEEDED((*member)->GetLevelDesc(0, &desc)) && desc.Height >= 2 && desc.Width == desc.Height * desc.Height;
	CellCount[slot] = strip ? (float)desc.Height : 0.0f;
	const bool neutral = !strip || IsIdentityLUT(*member);
	(slot == 0 ? DayNeutral : slot == 1 ? NightNeutral : InteriorNeutral) = neutral;
	if (!strip)
		Logger::Log("LUT %s is %ux%u, not a horizontal strip LUT (256x16, 1024x32 or 4096x64): it is ignored, "
			"because sampled as a strip it scrambles the colours.", filename, desc.Width, desc.Height);
	else if (neutral)
		Logger::Log("LUT %s is an identity LUT; the grading pass is skipped while only identity LUTs are in use.", filename);

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
	IDirect3DBaseTexture9* tex = GetLUTTexture(path);
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
		IDirect3DBaseTexture9* tex = GetLUTTexture(path);
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

bool LUTEffect::ShouldRender()
{
	// The shader returns lerp(color, graded, strength) with graded built from the LUTs it samples:
	// exteriors blend day and night, interiors use the interior LUT only (Blend.y). If every LUT
	// that can contribute is an identity the pass returns its input, so do not run it.
	if (Settings.Strength <= 0.0f) return false;
	if (TheShaderManager->GameState.isExterior)
		return !(DayNeutral && NightNeutral);
	return !InteriorNeutral;
}

void LUTEffect::UpdateConstants()
{
	Constants.Data.x = CellCount[0];
	Constants.Data.y = Settings.Strength;
	Constants.Data.z = CellCount[1];
	Constants.Data.w = CellCount[2];

	if (TheShaderManager->GameState.isExterior) {
		Constants.Blend.x = TheShaderManager->GameState.transitionCurve;
		Constants.Blend.y = 0.0f;
	} else {
		Constants.Blend.x = 0.0f;
		Constants.Blend.y = 1.0f;
	}
	Constants.Blend.z = Settings.HDRCompat ? 1.0f : 0.0f;
}
