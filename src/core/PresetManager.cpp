#include "PresetManager.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <unordered_set>
#include <vector>
#include <string_view>

std::unordered_map<std::string, std::string>	PresetManager::s_cellToKeyword;
std::unordered_map<std::string, UInt32>		PresetManager::s_keywordCellCount;
PresetManager::ResolveResult					PresetManager::s_lastResolveResult;

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
	Logger::Log("PresetManager: Initializing...");
	LoadKeywords();
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

	const fs::path keywordsPath = kKeywordsDir;
	if (!fs::exists(keywordsPath) || !fs::is_directory(keywordsPath)) {
		Logger::Log("PresetManager: Keywords directory not found: %s", kKeywordsDir);
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
			Logger::Log("PresetManager: Failed to open keyword file: %s", path.string().c_str());
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

	Logger::Log("PresetManager: Loaded %u cell keyword assignments across %u keyword(s)",
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

// ---- preset files --------------------------------------------------------
// docs § "Preset files": toml11-based, full standalone snapshot, never
// cached -- always read/written fresh off disk at the point of use.

std::string PresetManager::GetPresetPath(const std::string& Name) {
	return std::string(kPresetsDir) + Name + ".ini";
}

bool PresetManager::PresetExists(const std::string& Name) {
	return std::filesystem::exists(GetPresetPath(Name));
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

bool PresetManager::ReadPreset(const std::string& Name, PresetData& OutData) {
	OutData.clear();

	std::string path = GetPresetPath(Name);
	if (!std::filesystem::exists(path)) return false;

	try {
		tomlValue root = toml::parse<toml::preserve_comments, std::map>((std::string_view)path);
		WalkPresetTable(root, "", OutData);
	}
	catch (const std::exception& e) {
		Logger::Log("PresetManager: Failed to parse preset '%s': %s", Name.c_str(), e.what());
		return false;
	}

	return true;
}

bool PresetManager::WritePreset(const std::string& Name, const PresetData& Data) {
	tomlValue root = toml::table();

	for (const auto& [section, keys] : Data) {
		if (IsBlacklistedSection(section)) continue; // never write blacklisted content

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
			if (value.Type == PresetValue::ValueType::String)
				(*node)[key] = value.StringValue;
			else
				(*node)[key] = value.FloatValue;
		}
	}

	std::filesystem::create_directories(std::filesystem::path(kPresetsDir));

	std::ofstream file(GetPresetPath(Name), std::ios::trunc | std::ios::binary);
	if (!file.is_open()) {
		Logger::Log("PresetManager: Failed to open preset '%s' for writing", Name.c_str());
		return false;
	}

	file << root << std::endl;
	file.close();
	return true;
}

// ---- Session 2: resolution + apply ---------------------------------------
// docs § "Resolution order", § "Application mechanism"

bool PresetManager::ReadRawDefaults(PresetData& OutData) {
	OutData.clear();
	if (!TheSettingManager) return false;
	WalkPresetTable(TheSettingManager->Config.DefaultConfig, "", OutData);
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

	// Diff against the live value before writing -- SetSettingF/SetSettingS
	// each trigger a full SettingManager::LoadSettings() internally, so
	// skipping no-op writes matters here in a way it doesn't for the rare,
	// user-triggered RevertToSnapshot() this is modeled on.
	for (const auto& [section, keys] : Target) {
		for (const auto& [key, value] : keys) {
			if (value.Type == PresetValue::ValueType::String) {
				char buf[256] = {};
				TheSettingManager->GetSettingS(section.c_str(), key.c_str(), buf);
				if (value.StringValue != buf)
					TheSettingManager->SetSettingS(section.c_str(), key.c_str(), value.StringValue.c_str());
			}
			else {
				float current = TheSettingManager->GetSettingF(section.c_str(), key.c_str());
				if (current != value.FloatValue)
					TheSettingManager->SetSettingF(section.c_str(), key.c_str(), value.FloatValue);
			}
		}
	}

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

	ApplyPreset(target);
	s_lastResolveResult = result;

	Logger::Log("PresetManager: [Preset] Resolved %s -> tier=%d name='%s' rawDefaults=%d keyword='%s'",
		result.IsInterior ? result.CellEditorID.c_str() : result.WorldspaceEditorID.c_str(),
		(int)result.Tier, result.PresetName.c_str(), result.UsedRawDefaults, result.Keyword.c_str());
}

const PresetManager::ResolveResult& PresetManager::GetLastResolveResult() {
	return s_lastResolveResult;
}
