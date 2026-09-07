#include "PresetManager.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <unordered_set>
#include <vector>
#include <string_view>

// Mirrors SettingManager.cpp's own GetConfigBase() pattern: GetCurrentDirectoryA
// is reliable at startup but can be silently changed later by common-dialog
// calls (GetSaveFileNameA etc.) from anything else loaded in the process.
// Every path PresetManager touches gets resolved against this cached value,
// not the live CWD, for the same reason SettingManager already does this.
static const std::string& PresetManagerBase() {
	static std::string base;
	if (base.empty()) {
		char cwd[MAX_PATH] = {};
		GetCurrentDirectoryA(MAX_PATH, cwd);
		base = cwd;
		base += "\\";
	}
	return base;
}

static std::string ResolvePresetPath(const char* RelativePath) {
	return PresetManagerBase() + RelativePath;
}

std::unordered_map<std::string, std::string>	PresetManager::s_cellToKeyword;
std::unordered_map<std::string, UInt32>		PresetManager::s_keywordCellCount;
PresetManager::ResolveResult					PresetManager::s_lastResolveResult;
UInt32											PresetManager::s_resolveGeneration = 0;
PresetManager::PresetData						PresetManager::s_baselineSnapshot;

// ---- setting-scope blacklist ---------------------------------------------
// docs/preset-manager-design.md § "Setting scope -- a blacklist, still needed"
// Same excluded sections the rejected prototype got right: personal/system
// preferences, never eligible to live in a preset.
static const std::unordered_set<std::string> kBlacklistedSections = {
	"Main.CameraMode.Main",
	"Main.CameraMode.Positioning",
	"Main.Develop.Main",
	"Main.FlyCam.Main",
	"Main.FrameRate.SmartControl",
	"Main.FrameRate.Stuttering",
	"Main.LowHFSound.Main",
	"Main.Main.Misc",
	"Main.Menu.Keys",
	"Main.Menu.Style",
	"Main.SleepingMode.Main",
};

bool PresetManager::IsBlacklistedSection(const std::string& Section) {
	return kBlacklistedSections.count(Section) > 0;
}

bool PresetManager::IsReservedName(const std::string& Name) {
	return Name == kDefaultInteriorName || Name == kDefaultExteriorName;
}

// ---- init ------------------------------------------------------------------

void PresetManager::Initialize() {
	Logger::Log("PresetManager: [Preset] Initializing...");
	LoadKeywords();
	LoadEnabledVariants();
}

// ---- keyword files -----------------------------------------------------
// docs § "Keyword files": sections are keywords, cells are bare entries.
// Cached once at boot. Duplicate membership resolved deterministically:
// files processed alphabetically by filename, first found wins (docs
// § "Duplicate membership").

static std::string_view TrimView(std::string_view sv) {
	size_t start = sv.find_first_not_of(" \t\r\n");
	if (start == std::string_view::npos) return {};
	size_t end = sv.find_last_not_of(" \t\r\n");
	return sv.substr(start, end - start + 1);
}

void PresetManager::LoadKeywords() {
	namespace fs = std::filesystem;

	s_cellToKeyword.clear();
	s_keywordCellCount.clear();

	std::string resolvedDir = ResolvePresetPath(kKeywordsDir);
	const fs::path keywordsPath = resolvedDir;
	if (!fs::exists(keywordsPath) || !fs::is_directory(keywordsPath)) {
		Logger::Log("PresetManager: [Preset] Keywords directory not found: %s", resolvedDir.c_str());
		return;
	}

	// Deterministic order: alphabetical by filename (docs § "Duplicate membership").
	std::vector<fs::path> files;
	for (const auto& entry : fs::directory_iterator(keywordsPath)) {
		if (entry.is_regular_file() && entry.path().extension() == ".ini")
			files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());

	for (const auto& path : files) {
		std::ifstream file(path.string());
		if (!file.is_open()) {
			Logger::Log("PresetManager: [Preset] Failed to open keyword file: %s", path.string().c_str());
			continue;
		}

		std::string line;
		std::string currentKeyword;

		while (std::getline(file, line)) {
			std::string_view view = TrimView(line);

			if (view.empty() || view[0] == ';')
				continue;

			if (view.front() == '[' && view.back() == ']') {
				currentKeyword = std::string(view.substr(1, view.size() - 2));
				continue;
			}

			if (currentKeyword.empty())
				continue;

			std::string cellID(view);

			// sections within a file processed in the order they appear, and
			// files themselves alphabetically -- so "first found" here really
			// is deterministic, not directory-iteration-order dependent.
			if (s_cellToKeyword.find(cellID) != s_cellToKeyword.end())
				continue; // already assigned by an earlier file/section

			s_cellToKeyword[cellID] = currentKeyword;
			s_keywordCellCount[currentKeyword]++;
		}
	}

	Logger::Log("PresetManager: [Preset] Loaded %u cell keyword assignments across %u keyword(s)",
		(UInt32)s_cellToKeyword.size(), (UInt32)s_keywordCellCount.size());
}

const std::string* PresetManager::GetKeywordForCell(const std::string& CellEditorID) {
	auto it = s_cellToKeyword.find(CellEditorID);
	if (it == s_cellToKeyword.end()) return nullptr;
	return &it->second;
}

UInt32 PresetManager::CountCellsForKeyword(const std::string& Keyword) {
	auto it = s_keywordCellCount.find(Keyword);
	if (it == s_keywordCellCount.end()) return 0;
	return it->second;
}

std::vector<std::string> PresetManager::GetKnownKeywordCells() {
	std::vector<std::string> names;
	names.reserve(s_cellToKeyword.size());
	for (const auto& [cellID, keyword] : s_cellToKeyword)
		names.push_back(cellID);
	std::sort(names.begin(), names.end());
	return names;
}

// ---- preset files --------------------------------------------------------
// docs § "Preset files": toml11-based, full standalone snapshot, never
// cached -- always read/written fresh off disk at the point of use.

std::string PresetManager::GetPresetPath(const std::string& Name) {
	return ResolvePresetPath(kPresetsDir) + Name + ".ini";
}

std::string PresetManager::GetVariantPath(const std::string& Name) {
	return ResolvePresetPath(kVariantsDir) + Name + ".ini";
}

bool PresetManager::PresetExists(const std::string& Name) {
	return std::filesystem::exists(GetPresetPath(Name));
}

bool PresetManager::VariantExists(const std::string& Name) {
	return std::filesystem::exists(GetVariantPath(Name));
}

// Recursively walks a parsed toml table, reconstructing the dotted Section
// path SettingManager's own SetSettingF/SetSettingS expect (e.g.
// "Shaders.ImageAdjust.Main"), skipping blacklisted sections and any leaf
// type we don't support (arrays, sub-tables handled by recursion itself).
static void WalkPresetTable(const tomlValue& Node, const std::string& SectionPath, PresetManager::PresetData& Out) {
	if (!Node.is_table()) return;

	for (const auto& [key, child] : Node.as_table()) {
		if (child.is_table()) {
			// Top-level table keys in TheSettingManager->Config.DefaultConfig
			// carry a leading '_' (matching the raw TOML's [_Main...] section
			// headers) that SettingManager::Configuration::FillNode always
			// re-adds itself before every lookup -- strip it here so Section
			// strings we produce match the no-underscore convention
			// SetSettingF/GetSettingF's public API expects. Only matters when
			// walking DefaultConfig directly (see ReadRawDefaults); harmless
			// no-op for ordinary preset files, which never have this prefix.
			std::string segment = (SectionPath.empty() && !key.empty() && key.front() == '_')
				? key.substr(1) : key;
			std::string childPath = SectionPath.empty() ? segment : SectionPath + "." + segment;
			WalkPresetTable(child, childPath, Out);
			continue;
		}

		if (PresetManager::IsBlacklistedSection(SectionPath))
			continue;

		PresetManager::PresetValue value{};
		if (child.is_string()) {
			value.Type = PresetManager::PresetValue::ValueType::String;
			value.StringValue = child.as_string();
		}
		else if (child.is_boolean()) {
			value.Type = PresetManager::PresetValue::ValueType::Float;
			value.FloatValue = child.as_boolean() ? 1.0f : 0.0f;
		}
		else if (child.is_integer()) {
			value.Type = PresetManager::PresetValue::ValueType::Float;
			value.FloatValue = (float)child.as_integer();
		}
		else if (child.is_floating()) {
			value.Type = PresetManager::PresetValue::ValueType::Float;
			value.FloatValue = (float)child.as_floating();
		}
		else {
			continue; // unsupported leaf type (array, etc.) -- skip
		}

		Out[SectionPath][key] = value;
	}
}

// Shared by ReadPreset/ReadVariant -- same format, different folder/caller.
static bool ReadTomlPresetFile(const std::string& Path, const std::string& Label, PresetManager::PresetData& OutData) {
	OutData.clear();
	if (!std::filesystem::exists(Path)) return false;

	try {
		tomlValue root = toml::parse<toml::preserve_comments, std::map>((std::string_view)Path);
		WalkPresetTable(root, "", OutData);
	}
	catch (const std::exception& e) {
		Logger::Log("PresetManager: [Preset] Failed to parse '%s': %s", Label.c_str(), e.what());
		return false;
	}

	return true;
}

// Shared by WritePreset/WriteVariant -- same format, different folder/caller.
static bool WriteTomlPresetFile(const std::string& Path, const std::string& Dir, const std::string& Label, const PresetManager::PresetData& Data) {
	tomlValue root = toml::table();

	for (const auto& [section, keys] : Data) {
		if (PresetManager::IsBlacklistedSection(section)) continue; // never write blacklisted content

		StringList parts;
		SettingManager::SplitString(section.c_str(), ".", &parts);

		// Walk/create the nested table chain for this section -- same idiom as
		// SettingManager::Configuration::SetValue's own table-building code.
		tomlValue* node = &root;
		for (const auto& part : parts) {
			if (!node->contains(part)) (*node)[part] = toml::table();
			node = &node->at(part);
		}

		for (const auto& [key, value] : keys) {
			if (value.Type == PresetManager::PresetValue::ValueType::String)
				(*node)[key] = value.StringValue;
			else
				(*node)[key] = value.FloatValue;
		}
	}

	std::filesystem::create_directories(std::filesystem::path(Dir));

	std::ofstream file(Path, std::ios::trunc | std::ios::binary);
	if (!file.is_open()) {
		Logger::Log("PresetManager: [Preset] Failed to open '%s' for writing", Label.c_str());
		return false;
	}

	file << root << std::endl;
	file.close();
	return true;
}

bool PresetManager::ReadPreset(const std::string& Name, PresetData& OutData) {
	return ReadTomlPresetFile(GetPresetPath(Name), Name, OutData);
}

bool PresetManager::WritePreset(const std::string& Name, const PresetData& Data) {
	return WriteTomlPresetFile(GetPresetPath(Name), ResolvePresetPath(kPresetsDir), Name, Data);
}

bool PresetManager::ReadVariant(const std::string& Name, PresetData& OutData) {
	return ReadTomlPresetFile(GetVariantPath(Name), Name, OutData);
}

bool PresetManager::WriteVariant(const std::string& Name, const PresetData& Data) {
	return WriteTomlPresetFile(GetVariantPath(Name), ResolvePresetPath(kVariantsDir), Name, Data);
}

// ---- Session 2: resolution + apply ---------------------------------------
// docs § "Resolution order", § "Application mechanism"

bool PresetManager::ReadRawDefaults(PresetData& OutData) {
	OutData.clear();
	if (!TheSettingManager) return false;
	WalkPresetTable(TheSettingManager->Config.DefaultConfig, "", OutData);
	return true;
}

bool PresetManager::CaptureLiveState(PresetData& OutData) {
	OutData.clear();
	if (!TheSettingManager) return false;

	// The schema (which sections/keys exist, and each one's type) comes from
	// the defaults -- guaranteed complete. The VALUE for each comes from the
	// live getters, which correctly fall back to defaults for anything the
	// user's actual TomlConfig doesn't explicitly override, so this can't
	// silently miss an entry the way walking TomlConfig directly could.
	PresetData schema;
	if (!ReadRawDefaults(schema)) return false;

	for (const auto& [section, keys] : schema) {
		for (const auto& [key, defaultValue] : keys) {
			PresetValue live{};
			live.Type = defaultValue.Type;

			if (live.Type == PresetValue::ValueType::String) {
				char buf[256] = {};
				TheSettingManager->GetSettingS(section.c_str(), key.c_str(), buf);
				live.StringValue = buf;
			}
			else {
				live.FloatValue = TheSettingManager->GetSettingF(section.c_str(), key.c_str());
			}

			OutData[section][key] = live;
		}
	}

	return true;
}

bool PresetManager::ResolveCurrentLocation(TESObjectCELL* Cell, PresetData& OutTarget, ResolveResult& OutResult) {
	OutTarget.clear();
	OutResult = ResolveResult{};

	if (!Cell) return false;

	const char* cellEdID = Cell->GetEditorName();
	OutResult.CellEditorID = cellEdID ? cellEdID : "";
	OutResult.IsInterior = Cell->IsInterior();

	if (OutResult.IsInterior) {
		// 1. cell-specific Override always wins, checked regardless of keyword.
		if (!OutResult.CellEditorID.empty() && PresetExists(OutResult.CellEditorID)) {
			OutResult.Tier = ResolvedTier::Override;
			OutResult.PresetName = OutResult.CellEditorID;
			return ReadPreset(OutResult.PresetName, OutTarget);
		}

		// 2. Keyword preset, if the cell has a tag AND a matching preset exists.
		if (!OutResult.CellEditorID.empty()) {
			const std::string* keyword = GetKeywordForCell(OutResult.CellEditorID);
			if (keyword) {
				OutResult.Keyword = *keyword;
				if (PresetExists(*keyword)) {
					OutResult.Tier = ResolvedTier::Keyword;
					OutResult.PresetName = *keyword;
					return ReadPreset(OutResult.PresetName, OutTarget);
				}
			}
		}

		// 3. DefaultInterior, or raw TOML defaults if it hasn't been authored yet.
		OutResult.Tier = ResolvedTier::Default;
		if (PresetExists(kDefaultInteriorName)) {
			OutResult.PresetName = kDefaultInteriorName;
			return ReadPreset(OutResult.PresetName, OutTarget);
		}
		OutResult.UsedRawDefaults = true;
		return ReadRawDefaults(OutTarget);
	}
	else {
		TESWorldSpace* worldSpace = Cell->worldSpace;
		const char* wsEdID = worldSpace ? worldSpace->GetEditorName() : nullptr;
		OutResult.WorldspaceEditorID = wsEdID ? wsEdID : "";

		// 1. worldspace-specific Override always wins.
		if (!OutResult.WorldspaceEditorID.empty() && PresetExists(OutResult.WorldspaceEditorID)) {
			OutResult.Tier = ResolvedTier::Override;
			OutResult.PresetName = OutResult.WorldspaceEditorID;
			return ReadPreset(OutResult.PresetName, OutTarget);
		}

		// 2. DefaultExterior, or raw TOML defaults. No keyword tier for
		// exteriors (docs § "Resolution order").
		OutResult.Tier = ResolvedTier::Default;
		if (PresetExists(kDefaultExteriorName)) {
			OutResult.PresetName = kDefaultExteriorName;
			return ReadPreset(OutResult.PresetName, OutTarget);
		}
		OutResult.UsedRawDefaults = true;
		return ReadRawDefaults(OutTarget);
	}
}

void PresetManager::ApplyPreset(const PresetData& Target) {
	if (!TheSettingManager) return;

	// Whatever gets pushed here becomes the new "since step 1 began"
	// reference point for Session 7's Save Variant diff (docs § "Variants"
	// authoring flow) -- this one choke point covers both the normal
	// resolve path (ResolveAndApply) and a Session 6 Load preview
	// (ApplyPreviewPreset), matching "Load any base preset -- doesn't
	// matter which."
	s_baselineSnapshot = Target;

	// Diff against the live value before writing -- SetSettingF/SetSettingS
	// each trigger a full SettingManager::LoadSettings() internally, so
	// skipping no-op writes matters here in a way it doesn't for the rare,
	// user-triggered RevertToSnapshot() this is modeled on.
	//
	// TEMP DIAGNOSTIC (remove once the "presets resolve/save correctly but
	// don't visibly apply" report is root-caused): counts + a few example
	// writes, so the next repro's log says definitively whether this loop is
	// actually writing anything, or silently diffing everything away.
	UInt32 consideredCount = 0, writtenCount = 0, loggedExamples = 0;
	for (const auto& [section, keys] : Target) {
		for (const auto& [key, value] : keys) {
			consideredCount++;
			if (value.Type == PresetValue::ValueType::String) {
				char buf[256] = {};
				TheSettingManager->GetSettingS(section.c_str(), key.c_str(), buf);
				if (value.StringValue != buf) {
					TheSettingManager->SetSettingS(section.c_str(), key.c_str(), value.StringValue.c_str());
					writtenCount++;
					if (loggedExamples < 5) {
						Logger::Log("PresetManager: [Preset]   apply %s.%s: '%s' -> '%s'",
							section.c_str(), key.c_str(), buf, value.StringValue.c_str());
						loggedExamples++;
					}
				}
			}
			else {
				float current = TheSettingManager->GetSettingF(section.c_str(), key.c_str());
				if (current != value.FloatValue) {
					TheSettingManager->SetSettingF(section.c_str(), key.c_str(), value.FloatValue);
					writtenCount++;
					if (loggedExamples < 5) {
						Logger::Log("PresetManager: [Preset]   apply %s.%s: %g -> %g",
							section.c_str(), key.c_str(), current, value.FloatValue);
						loggedExamples++;
					}
				}
			}
		}
	}
	Logger::Log("PresetManager: [Preset] ApplyPreset: %u key(s) considered, %u actually written",
		consideredCount, writtenCount);

	// Re-sync shader Enabled flags -- same pattern as ImGuiManager.cpp's
	// RevertToSnapshot(), since a preset can toggle Shaders.*.Status.Enabled.
	StringList shaders;
	TheSettingManager->FillMenuSections(&shaders, "Shaders");
	for (const auto& name : shaders) {
		bool want = TheSettingManager->GetMenuShaderEnabled(name.c_str());
		EffectRecord* effect = TheShaderManager->GetEffectByName(name.c_str());
		if (effect) { effect->Enabled = want; continue; }
		ShaderCollection* shader = TheShaderManager->GetShaderCollectionByName(name.c_str());
		if (shader) shader->Enabled = want;
	}

	TheSettingManager->LoadSettings(); // final refresh, matching RevertToSnapshot's own pattern
}

void PresetManager::ResolveAndApply(TESObjectCELL* Cell) {
	PresetData target;
	ResolveResult result;

	if (!ResolveCurrentLocation(Cell, target, result))
		return;

	ApplyVariants(target);

	ApplyPreset(target);
	s_lastResolveResult = result;
	s_resolveGeneration++;

	Logger::Log("PresetManager: [Preset] Resolved %s -> tier=%d name='%s' rawDefaults=%d keyword='%s' variants=%u",
		result.IsInterior ? result.CellEditorID.c_str() : result.WorldspaceEditorID.c_str(),
		(int)result.Tier, result.PresetName.c_str(), result.UsedRawDefaults, result.Keyword.c_str(),
		(UInt32)s_enabledVariants.size());
}

const PresetManager::ResolveResult& PresetManager::GetLastResolveResult() {
	return s_lastResolveResult;
}

// ---- Session 6: Preset browser / Load -------------------------------------
// docs § "In-game UI -- Preset browser / Load", § "Unsaved/previewing state"

std::vector<PresetManager::PresetListEntry> PresetManager::ListAllPresets() {
	namespace fs = std::filesystem;
	std::vector<PresetListEntry> entries;

	std::string resolvedDir = ResolvePresetPath(kPresetsDir);
	const fs::path presetsPath = resolvedDir;
	if (!fs::exists(presetsPath) || !fs::is_directory(presetsPath))
		return entries;

	for (const auto& entry : fs::directory_iterator(presetsPath)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".ini")
			continue;

		std::string name = entry.path().stem().string();
		PresetKind kind;
		if (name == kDefaultInteriorName || name == kDefaultExteriorName)
			kind = PresetKind::Default;
		else if (s_keywordCellCount.find(name) != s_keywordCellCount.end())
			kind = PresetKind::Keyword;
		else
			kind = PresetKind::Override;

		entries.push_back({ name, kind });
	}

	// Keyword group first, then Default, then Override (docs: "Keyword
	// presets are grouped first"); alphabetical within each group.
	std::sort(entries.begin(), entries.end(), [](const PresetListEntry& a, const PresetListEntry& b) {
		if (a.Kind != b.Kind) return (int)a.Kind < (int)b.Kind;
		return a.Name < b.Name;
	});

	return entries;
}

void PresetManager::ApplyPreviewPreset(const PresetData& Data) {
	ApplyPreset(Data);
}

UInt32 PresetManager::GetResolveGeneration() {
	return s_resolveGeneration;
}

// ---- Session 4: Variants -------------------------------------------------
// docs § "Variants", § "Persisting which Variants are enabled"

std::vector<std::string> PresetManager::s_enabledVariants;

void PresetManager::ApplyVariants(PresetData& Target) {
	// Order is priority: later-enabled Variants unconditionally overwrite
	// earlier ones (and the base preset) for any key they define.
	for (const auto& variantName : s_enabledVariants) {
		if (!VariantExists(variantName)) continue;

		PresetData variantData;
		if (!ReadVariant(variantName, variantData)) continue;

		for (const auto& [section, keys] : variantData)
			for (const auto& [key, value] : keys)
				Target[section][key] = value;
	}
}

void PresetManager::LoadEnabledVariants() {
	s_enabledVariants.clear();

	std::ifstream file(ResolvePresetPath(kEnabledVariantsPath));
	if (!file.is_open()) {
		Logger::Log("PresetManager: [Preset] EnabledVariants.ini not found (no Variants enabled yet)");
		return;
	}

	std::string line;
	bool inSection = false;

	while (std::getline(file, line)) {
		std::string_view view = TrimView(line);

		if (view.empty() || view[0] == ';')
			continue;

		if (view.front() == '[' && view.back() == ']') {
			inSection = (view == "[EnabledVariants]");
			continue;
		}

		if (!inSection) continue;

		std::string name(view);
		if (std::find(s_enabledVariants.begin(), s_enabledVariants.end(), name) == s_enabledVariants.end())
			s_enabledVariants.push_back(name);
	}

	Logger::Log("PresetManager: [Preset] Loaded %u enabled Variant(s)", (UInt32)s_enabledVariants.size());
}

void PresetManager::SaveEnabledVariants() {
	std::filesystem::create_directories(std::filesystem::path(ResolvePresetPath("Data\\NVR\\")));

	std::ofstream file(ResolvePresetPath(kEnabledVariantsPath), std::ios::trunc | std::ios::binary);
	if (!file.is_open()) {
		Logger::Log("PresetManager: [Preset] Failed to open EnabledVariants.ini for writing");
		return;
	}

	file << "[EnabledVariants]\n";
	for (const auto& name : s_enabledVariants)
		file << name << "\n";
}

const std::vector<std::string>& PresetManager::GetEnabledVariants() {
	return s_enabledVariants;
}

void PresetManager::SetVariantEnabled(const std::string& Name, bool Enabled) {
	auto it = std::find(s_enabledVariants.begin(), s_enabledVariants.end(), Name);

	if (Enabled) {
		if (it == s_enabledVariants.end())
			s_enabledVariants.push_back(Name); // append to bottom -- becomes highest priority
	}
	else if (it != s_enabledVariants.end()) {
		s_enabledVariants.erase(it); // removed entirely, not just disabled
	}

	SaveEnabledVariants();
}

// ---- Session 7: Variants UI, Refresh All Presets ---------------------------
// docs § "In-game UI -- Variants", § "Refresh all presets"

std::vector<std::string> PresetManager::ListAllVariants() {
	namespace fs = std::filesystem;
	std::vector<std::string> names;

	std::string resolvedDir = ResolvePresetPath(kVariantsDir);
	const fs::path variantsPath = resolvedDir;
	if (!fs::exists(variantsPath) || !fs::is_directory(variantsPath))
		return names;

	for (const auto& entry : fs::directory_iterator(variantsPath)) {
		if (entry.is_regular_file() && entry.path().extension() == ".ini")
			names.push_back(entry.path().stem().string());
	}
	std::sort(names.begin(), names.end());

	return names;
}

void PresetManager::CaptureVariantDiff(PresetData& OutDiff) {
	OutDiff.clear();

	PresetData live;
	if (!CaptureLiveState(live)) return;

	for (const auto& [section, keys] : live) {
		auto baseSectionIt = s_baselineSnapshot.find(section);

		for (const auto& [key, value] : keys) {
			bool changed = true; // no baseline value to compare against -> conservatively "changed"

			if (baseSectionIt != s_baselineSnapshot.end()) {
				auto baseKeyIt = baseSectionIt->second.find(key);
				if (baseKeyIt != baseSectionIt->second.end() && baseKeyIt->second.Type == value.Type) {
					changed = (value.Type == PresetValue::ValueType::String)
						? (baseKeyIt->second.StringValue != value.StringValue)
						: (baseKeyIt->second.FloatValue != value.FloatValue);
				}
			}

			if (changed) OutDiff[section][key] = value;
		}
	}
}

std::vector<PresetManager::RefreshSummary> PresetManager::RefreshAllPresets() {
	std::vector<RefreshSummary> summaries;

	PresetData eligible; // current defaults, minus the blacklist -- the "universe" (docs step 1)
	if (!ReadRawDefaults(eligible)) return summaries;

	for (const auto& entry : ListAllPresets()) {
		PresetData existing;
		if (!ReadPreset(entry.Name, existing)) continue;

		RefreshSummary summary;
		summary.PresetName = entry.Name;
		bool changed = false;

		for (const auto& [section, keys] : eligible) {
			auto existingSectionIt = existing.find(section);
			for (const auto& [key, defaultValue] : keys) {
				bool alreadyHasKey = existingSectionIt != existing.end()
					&& existingSectionIt->second.find(key) != existingSectionIt->second.end();
				if (alreadyHasKey) continue; // never overwrite an existing customization (step 3)

				existing[section][key] = defaultValue;
				existingSectionIt = existing.find(section); // existing[section] may have just been created
				changed = true;

				char line[320];
				if (defaultValue.Type == PresetValue::ValueType::String)
					snprintf(line, sizeof(line), "%s.%s = %s", section.c_str(), key.c_str(), defaultValue.StringValue.c_str());
				else
					snprintf(line, sizeof(line), "%s.%s = %g", section.c_str(), key.c_str(), defaultValue.FloatValue);
				summary.AddedKeys.push_back(line);
			}
		}

		// No pruning (docs step 4) -- `existing` is only ever added to above,
		// never trimmed, so any stale key it already had survives untouched.
		if (changed && WritePreset(entry.Name, existing))
			summaries.push_back(std::move(summary));
	}

	return summaries;
}
