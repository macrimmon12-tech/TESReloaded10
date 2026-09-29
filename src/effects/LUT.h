#pragma once

class LUTEffect : public EffectRecord
{
public:
	LUTEffect() : EffectRecord("LUT") {};

	struct LUTStruct {
		D3DXVECTOR4 Data;   // x=day N (cell size/count), y=strength, z=night N, w=interior N (0 = no usable LUT)
		D3DXVECTOR4 Blend;  // x=dayNightLerp (0=night, 1=day), y=isInterior (0 or 1)
	};

	struct LUTSettingsStruct {
		float Strength;
		bool  PreTonemapping;
		bool  HDRCompat;
	};

	LUTStruct          Constants;
	LUTSettingsStruct  Settings;

	IDirect3DTexture9* DayTexture      = nullptr;
	IDirect3DTexture9* NightTexture    = nullptr;
	IDirect3DTexture9* InteriorTexture = nullptr;

	std::vector<std::string> LUTFiles;
	int DayIdx      = 0;
	int NightIdx    = 0;
	int InteriorIdx = 0;

	void RegisterConstants();
	void RegisterTextures();
	void UpdateSettings();
	void UpdateConstants();
	bool ShouldRender() override;

	void ScanLUTFolder();

	// Assigns an already-loaded texture to a slot (member pointer, sampler binding,
	// the slot's cell count and strip check, LUTFiles index sync). Does not load or persist anything —
	// loading is TextureManager::GetFileTexture()'s job, persistence is SaveLUTSetting()'s.
	void AssignLUTSlot(int slot, IDirect3DBaseTexture9* texture, const char* filename); // slot: 0=day, 1=night, 2=interior

	// Persists the chosen filename for a slot back to settings.
	void SaveLUTSetting(int slot, const char* filename); // slot: 0=day, 1=night, 2=interior

	// Convenience orchestrator for a user-driven LUT pick: loads the file via
	// TheTextureManager->GetFileTexture(), assigns it to the slot, and saves the choice.
	void LoadLUT(int slot, const char* filename); // slot: 0=day, 1=night, 2=interior

	static const char* LUTFolder;

private:
	// Cell count N of each slot's strip LUT (day, night, interior), passed to the shader per slot. 0 means
	// the slot has no usable LUT (file missing or not an N*N x N strip): the shader passes colours through.
	float CellCount[3] = {};

	// Set when the slot's texture is an identity strip, or when the slot has no usable LUT. A pass that
	// only sees such slots returns its input, so ShouldRender skips it (the shipped neutral_lut.png is one).
	bool DayNeutral      = true;
	bool NightNeutral    = true;
	bool InteriorNeutral = true;
};
