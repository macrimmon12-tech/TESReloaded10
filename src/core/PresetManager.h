#pragma once

#include <string>
#include <unordered_map>
#include <vector>
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
	static constexpr const char* kVariantsDir         = "Data\\NVR\\Variants\\";
	static constexpr const char* kEnabledVariantsPath = "Data\\NVR\\EnabledVariants.ini";
	static constexpr const char* kDefaultInteriorName = "DefaultInterior";
	static constexpr const char* kDefaultExteriorName = "DefaultExterior";

	static void			Initialize();

	// Keyword membership -- read and cached once at boot (docs § "Keyword files").
	// Interior cells only; worldspaces never appear here.
	static const std::string*	GetKeywordForCell(const std::string& CellEditorID);
	static UInt32				CountCellsForKeyword(const std::string& Keyword);

	// Every interior cell EditorID mentioned in any keyword file, sorted --
	// used by the Dev Tools coc/cow picker (a general utility, unrelated to
	// presets, docs § "Scope") as one real source of "known cells" to browse,
	// alongside whatever the player has actually visited this session.
	static std::vector<std::string>	GetKnownKeywordCells();

	// Preset file I/O. Always reads/writes fresh off disk -- never cached
	// (docs § "Preset files"). Name is the bare preset name, no path/extension
	// (e.g. "DefaultInterior", "HonestHeartsCave", a cell/worldspace EditorID).
	static bool			ReadPreset(const std::string& Name, PresetData& OutData);
	static bool			WritePreset(const std::string& Name, const PresetData& Data);
	static bool			PresetExists(const std::string& Name);

	// Variant preset I/O -- same format, own folder (docs § "Folder layout").
	// WriteVariant has no caller yet; Session 7's Save Variant flow adds one.
	static bool			ReadVariant(const std::string& Name, PresetData& OutData);
	static bool			WriteVariant(const std::string& Name, const PresetData& Data);
	static bool			VariantExists(const std::string& Name);

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

	// ---- Session 5: authoring (docs § "In-game UI -- location assignment") --

	// Captures every non-blacklisted setting's CURRENT effective value (not a
	// diff) -- enumerates the full schema from raw TOML defaults, then reads
	// each key's live value via GetSettingF/GetSettingS (correctly falls back
	// to defaults for anything the live TomlConfig doesn't explicitly
	// override, unlike walking TomlConfig directly which could miss entries
	// a sparse user config omits). This is what a Save button writes.
	static bool			CaptureLiveState(PresetData& OutData);

	// ---- Session 6: Preset browser / Load (docs § "In-game UI -- Preset
	// browser / Load") -----------------------------------------------------

	// Ordering matches the docs' display order -- Keyword presets grouped
	// first, ahead of Default and Override -- since the enum's own ordinal
	// is what ListAllPresets() sorts by.
	enum class PresetKind { Keyword, Default, Override };
	struct PresetListEntry {
		std::string	Name;
		PresetKind	Kind;
	};

	// Enumerates Data\NVR\Presets\ fresh off disk every call -- same
	// never-cached philosophy as ReadPreset itself, so a preset saved a
	// moment ago via the authoring UI shows up immediately. Classifies each
	// by name alone (cross-linked by name only, no per-file metadata):
	// DefaultInterior/DefaultExterior -> Default, a name matching a known
	// keyword -> Keyword, everything else -> Override.
	static std::vector<PresetListEntry>	ListAllPresets();

	// Pushes Data into the engine exactly like the normal resolve path does,
	// but deliberately does NOT touch s_lastResolveResult or the resolve
	// generation counter below -- Load previews a preset's contents without
	// redefining what's actually assigned to the current location (docs §
	// "Unsaved/previewing state"). Walking away without saving silently
	// reverts because the next real ResolveAndApply overwrites it.
	static void				ApplyPreviewPreset(const PresetData& Data);

	// Bumped once per actual ResolveAndApply call (i.e. once per real cell
	// transition). The in-game UI captures this value when a Load preview
	// starts and compares it every frame -- a mismatch means the player
	// walked to a different location, so the preview should be silently
	// dropped per the docs, with no extra plumbing needed to detect that.
	static UInt32			GetResolveGeneration();

	// ---- Session 4: Variants (docs § "Variants",
	// § "Persisting which Variants are enabled") -------------------------

	// Ordered list of currently-enabled Variant names -- order is meaningful,
	// not incidental: it's the same order used to break conflicts between
	// simultaneously-enabled Variants (later entries win). Read once at boot
	// alongside the keyword files.
	static const std::vector<std::string>&	GetEnabledVariants();

	// Toggles one Variant on/off and writes EnabledVariants.ini immediately --
	// enabling appends to the bottom of the list (becomes highest priority);
	// disabling removes the entry entirely, so re-enabling later appends
	// fresh at the bottom again.
	static void				SetVariantEnabled(const std::string& Name, bool Enabled);

	// ---- Session 7: Variants UI, Refresh All Presets, Dev Tools ------------

	// Every Variant file on disk (Data\NVR\Variants\*.ini), alphabetical --
	// unlike GetEnabledVariants() this includes disabled ones too, since the
	// in-game checkbox panel needs to list all of them, not just active ones.
	static std::vector<std::string>	ListAllVariants();

	// Diffs the current live settings against the baseline snapshot captured
	// by the most recent ApplyPreset call (i.e. whatever base preset is
	// currently live, however it got there -- normal resolution or a Session
	// 6 Load) and returns only the keys that differ, as absolute values
	// (docs § "Variants" authoring flow steps 2-3). A key present live but
	// absent from the baseline (an out-of-date preset file predating that
	// setting) is conservatively treated as changed, since there's no
	// baseline value to compare it against.
	static void				CaptureVariantDiff(PresetData& OutDiff);

	// One touched-file's summary for a Refresh All Presets run (docs §
	// "Refresh all presets"), logged by the caller.
	struct RefreshSummary {
		std::string					PresetName;
		std::vector<std::string>	AddedKeys; // "Section.Key = value", already formatted
	};

	// For every Default/Keyword/Override preset file: adds any eligible key
	// (from the current defaults, minus the blacklist) the file doesn't
	// already have, at the current default value. Never touches a key the
	// file already has, and never prunes a stale one (docs § "Refresh all
	// presets" -- no implicit apply-time fallback, no back-compat break).
	// Returns one entry per file actually touched; untouched files are
	// omitted entirely.
	static std::vector<RefreshSummary>	RefreshAllPresets();

private:
	static void			LoadKeywords();
	static std::string	GetPresetPath(const std::string& Name);
	static std::string	GetVariantPath(const std::string& Name);

	// Raw TOML defaults (TheSettingManager->Config.DefaultConfig), reshaped
	// into our own PresetData -- used when DefaultInterior/DefaultExterior
	// hasn't been authored yet (docs § "Resolution order").
	static bool			ReadRawDefaults(PresetData& OutData);

	static bool			ResolveCurrentLocation(TESObjectCELL* Cell, PresetData& OutTarget, ResolveResult& OutResult);
	static void			ApplyPreset(const PresetData& Target);

	// Layers each enabled Variant's diff onto Target in order, later Variants
	// unconditionally overwriting earlier ones (and the base preset) for any
	// key they define (docs § "Application mechanism" step 2).
	static void			ApplyVariants(PresetData& Target);

	static void			LoadEnabledVariants();
	static void			SaveEnabledVariants();

	// cell EditorID -> keyword name
	static std::unordered_map<std::string, std::string>	s_cellToKeyword;
	// keyword name -> number of cells currently tagged with it
	static std::unordered_map<std::string, UInt32>			s_keywordCellCount;
	// ordered (file order = priority order), no duplicates
	static std::vector<std::string>						s_enabledVariants;

	static ResolveResult	s_lastResolveResult;
	static UInt32			s_resolveGeneration;

	// Whatever ApplyPreset most recently pushed into the engine -- the
	// "since step 1 began" reference point CaptureVariantDiff() compares
	// live settings against (docs § "Variants" authoring flow).
	static PresetData		s_baselineSnapshot;
};
