#pragma once

#include <string>
#include <unordered_map>
#include "SettingManager.h" // reuses the tomlValue typedef and toml11 setup

/*
* docs/preset-manager-design.md. Session 1: keyword-file parsing/caching,
* preset file read/write, the setting-scope blacklist -- pure data layer, no
* game-loop/UI integration. Session 2 (this file's ResolveAndApply et al.):
* resolution + apply, wired into ShaderManager::UpdateConstants(), plus a
* minimal read-only debug window in ImGuiManager.cpp. See § "Implementation
* session plan" for what's still ahead.
*/
class PresetManager {
public:
	// A single preset's parsed content, keyed the same way SettingManager's own
	// SetSettingF/SetSettingS take Section (a full dotted path, e.g.
	// "Shaders.ImageAdjust.Main") and Key. Bools/ints/floats all collapse into
	// FloatValue -- SetSettingF already does the correct typed conversion based
	// on the live setting's actual type, same as the rejected prototype did.
	struct PresetValue {
		enum class ValueType { Float, String } Type;
		float		FloatValue;
		std::string	StringValue;
	};
	typedef std::unordered_map<std::string, PresetValue>	PresetSection; // Key -> Value
	typedef std::unordered_map<std::string, PresetSection>	PresetData;    // Section -> Keys

	static constexpr const char* kKeywordsDir         = "Data\\NVR\\Keywords\\";
	static constexpr const char* kPresetsDir          = "Data\\NVR\\Presets\\";
	static constexpr const char* kDefaultInteriorName = "DefaultInterior";
	static constexpr const char* kDefaultExteriorName = "DefaultExterior";

	static void			Initialize();

	// Keyword membership -- read and cached once at boot (docs § "Keyword files").
	// Interior cells only; worldspaces never appear here.
	static const std::string*	GetKeywordForCell(const std::string& CellEditorID);
	static UInt32				CountCellsForKeyword(const std::string& Keyword);

	// Preset file I/O. Always reads/writes fresh off disk -- never cached
	// (docs § "Preset files"). Name is the bare preset name, no path/extension
	// (e.g. "DefaultInterior", "HonestHeartsCave", a cell/worldspace EditorID).
	static bool			ReadPreset(const std::string& Name, PresetData& OutData);
	static bool			WritePreset(const std::string& Name, const PresetData& Data);
	static bool			PresetExists(const std::string& Name);

	// Setting-scope blacklist (docs § "Setting scope -- a blacklist, still
	// needed"). Section is the full dotted path, e.g. "Main.CameraMode.Main".
	static bool			IsBlacklistedSection(const std::string& Section);

	// DefaultInterior/DefaultExterior are reserved -- a keyword can't be named
	// either (docs § "Folder layout").
	static bool			IsReservedName(const std::string& Name);

	// ---- Session 2: resolution + apply (docs § "Resolution order",
	// § "Application mechanism") ----------------------------------------

	enum class ResolvedTier { Default, Keyword, Override };

	// Everything the debug window (and later, the real status indicators)
	// need to display -- populated as a side effect of ResolveAndApply.
	struct ResolveResult {
		ResolvedTier	Tier             = ResolvedTier::Default;
		std::string		PresetName;      // filename used, empty if raw-defaults fallback
		bool			UsedRawDefaults  = false; // docs: missing Default file falls back to raw TOML defaults
		std::string		Keyword;         // cell's assigned keyword, if any (informational)
		std::string		CellEditorID;
		std::string		WorldspaceEditorID;
		bool			IsInterior       = false;
	};

	// Called once per actual cell transition (gated on GameState.isCellChanged
	// at the call site -- see ShaderManager::UpdateConstants()). Resolves the
	// winning preset for Cell and applies it via TheSettingManager directly.
	static void					ResolveAndApply(TESObjectCELL* Cell);
	static const ResolveResult&	GetLastResolveResult();

private:
	static void			LoadKeywords();
	static std::string	GetPresetPath(const std::string& Name);

	// Raw TOML defaults (TheSettingManager->Config.DefaultConfig), reshaped
	// into our own PresetData -- used when DefaultInterior/DefaultExterior
	// hasn't been authored yet (docs § "Resolution order").
	static bool			ReadRawDefaults(PresetData& OutData);

	static bool			ResolveCurrentLocation(TESObjectCELL* Cell, PresetData& OutTarget, ResolveResult& OutResult);
	static void			ApplyPreset(const PresetData& Target);

	// cell EditorID -> keyword name
	static std::unordered_map<std::string, std::string>	s_cellToKeyword;
	// keyword name -> number of cells currently tagged with it
	static std::unordered_map<std::string, UInt32>			s_keywordCellCount;

	static ResolveResult	s_lastResolveResult;
};
