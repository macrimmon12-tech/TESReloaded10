#include "ImGuiManager.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include <sstream>
#include <iomanip>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool    ImGuiManager::Initialized     = false;
bool    ImGuiManager::Visible         = false;
bool    ImGuiManager::DeviceLost      = false;
HWND    ImGuiManager::GameWindow      = nullptr;
WNDPROC ImGuiManager::OriginalWndProc = nullptr;

static int         CursorShowDelta    = 0;
static std::string SelectedSection;
static std::string HoveredDescription;
static bool        InFileDialog       = false;

// Flags we disable on the player while the overlay is open.
static constexpr UInt32 kOverlayControlFlags =
	TogglePlayerControlsAlt::kFlag_Movement   |
	TogglePlayerControlsAlt::kFlag_Looking    |
	TogglePlayerControlsAlt::kFlag_Fighting   |
	TogglePlayerControlsAlt::kFlag_Attacking  |
	TogglePlayerControlsAlt::kFlag_AimingOrBlocking |
	TogglePlayerControlsAlt::kFlag_EnterVATS  |
	TogglePlayerControlsAlt::kFlag_Jumping    |
	TogglePlayerControlsAlt::kFlag_POV;

// ---- xNVSE input block -------------------------------------------------------

static void BlockGameInput(bool block) {
	// Block/unblock all mouse buttons and wheel via DIHookControl so clicks and
	// scroll events don't reach the game while the overlay is open.
	if (g_DIHookCtrl) {
		for (UInt32 code = kMacro_MouseButtonOffset; code < kMaxMacros; code++)
			g_DIHookCtrl->SetKeyDisableState(code, block, DIHookControl::kDisable_User);
	}

	// Disable/enable player controls (camera, movement, etc.) so the character
	// doesn't react to mouse deltas or WASD while the overlay is open.
	if (g_PlayerControls) {
		if (block)
			g_PlayerControls->DisablePlayerControlsAlt(kOverlayControlFlags, "NVR");
		else
			g_PlayerControls->EnablePlayerControlsAlt(kOverlayControlFlags, "NVR");
	}
}

// ---- Cursor / visibility -----------------------------------------------------

static void SetOverlayVisible(bool visible) {
	if (ImGuiManager::IsVisible() == visible) return;
	ImGuiManager::SetVisible(visible);
	if (visible) {
		// ShowCursor uses a reference counter; track every TRUE call so we can
		// undo exactly that many FALSE calls on hide.
		CursorShowDelta = 0;
		int count;
		do { count = ShowCursor(TRUE); CursorShowDelta++; } while (count < 0);
		ClipCursor(nullptr);
		BlockGameInput(true);
		ImGui::GetIO().ClearInputKeys();
	} else {
		BlockGameInput(false);
		for (int i = 0; i < CursorShowDelta; i++) ShowCursor(FALSE);
		CursorShowDelta = 0;
	}
}

// ---- WndProc -----------------------------------------------------------------

LRESULT CALLBACK ImGuiManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE && !InFileDialog)
		SetOverlayVisible(false);

	// FNV doesn't call TranslateMessage so WM_CHAR is never posted.
	// Manually convert WM_KEYDOWN to characters when ImGui needs text input.
	if (Visible && msg == WM_KEYDOWN && ImGui::GetIO().WantTextInput) {
		BYTE ks[256];
		GetKeyboardState(ks);
		WCHAR buf[4] = {};
		int n = ToUnicode((UINT)wParam, (lParam >> 16) & 0xFF, ks, buf, 4, 0);
		for (int i = 0; i < n; i++)
			ImGui::GetIO().AddInputCharacterUTF16(buf[i]);
	}

	if (Visible) {
		if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
			return TRUE;
		// When the overlay is open, keep the cursor visible everywhere in the window.
		// ImGui only handles WM_SETCURSOR when the mouse is over one of its windows;
		// if the mouse is in the game viewport the original WndProc would hide it.
		if (msg == WM_SETCURSOR) {
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
			return TRUE;
		}
	}
	return CallWindowProc(OriginalWndProc, hwnd, msg, wParam, lParam);
}

// ---- Init / shutdown ---------------------------------------------------------

void ImGuiManager::Initialize() {
	if (Initialized) return;

	IDirect3DDevice9* device = TheRenderManager->device;
	HWND hwnd = TheRenderManager->m_kWndFocus;
	if (!device || !hwnd) { Logger::Log("ImGuiManager: deferring init"); return; }

	GameWindow = hwnd;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.IniFilename = nullptr;

	ImGui::StyleColorsDark();
	ImGui::GetStyle().WindowRounding    = 6.0f;
	ImGui::GetStyle().FrameRounding     = 3.0f;
	ImGui::GetStyle().ScrollbarRounding = 3.0f;

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX9_Init(device);

	OriginalWndProc = (WNDPROC)SetWindowLong(hwnd, GWL_WNDPROC, (LONG)(LONG_PTR)WndProc);

	Initialized = true;
	Logger::Log("ImGuiManager: Initialized");
}

void ImGuiManager::Shutdown() {
	if (!Initialized) return;
	SetOverlayVisible(false);
	SetWindowLong(GameWindow, GWL_WNDPROC, (LONG)(LONG_PTR)OriginalWndProc);
	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	Initialized = false;
	Logger::Log("ImGuiManager: Shutdown");
}

// ---- Frame -------------------------------------------------------------------

void ImGuiManager::NewFrame() {
	if (!Initialized) { Initialize(); if (!Initialized) return; }

	HRESULT coop = TheRenderManager->device->TestCooperativeLevel();
	if (coop == D3DERR_DEVICELOST) {
		if (!DeviceLost) { ImGui_ImplDX9_InvalidateDeviceObjects(); DeviceLost = true; }
		return;
	}
	if (DeviceLost && coop == D3DERR_DEVICENOTRESET) {
		ImGui_ImplDX9_CreateDeviceObjects();
		DeviceLost = false;
	}

	// Close if a game menu is active (inventory, pip-boy, etc.)
	if (Visible && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_None))
		SetOverlayVisible(false);

	// Toggle via the configured key (same DirectInput keycode the old menu used)
	if (TheSettingManager && Global && Global->OnKeyDown(TheSettingManager->SettingsMain.Menu.KeyEnable))
		SetOverlayVisible(!Visible);

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	if (Visible) {
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
		io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
		io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
	}
}

void ImGuiManager::Render() {
	if (!Initialized || DeviceLost) return;
	BuildUI();
	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

// ---- Menu UI -----------------------------------------------------------------

static void RenderSetting(SettingManager::Configuration::ConfigNode& node) {
	using NodeType = SettingManager::Configuration::NodeType;

	ImGui::PushID(node.Key);

	switch (node.Type) {
	case NodeType::Boolean: {
		bool val = (strcmp(node.Value, "1") == 0 || _stricmp(node.Value, "true") == 0);
		if (ImGui::Checkbox(node.Key, &val)) {
			TheSettingManager->SetSetting(node.Section, node.Key, val);
			TheSettingManager->LoadSettings();
		}
		break;
	}
	case NodeType::Float: {
		float val = (float)atof(node.Value);
		if (ImGui::DragFloat(node.Key, &val, 0.001f, 0.0f, 0.0f, "%.4f")) {
			TheSettingManager->SetSetting(node.Section, node.Key, val);
			TheSettingManager->LoadSettings();
		}
		break;
	}
	case NodeType::Integer: {
		int val = atoi(node.Value);
		if (ImGui::DragInt(node.Key, &val, 1.0f)) {
			TheSettingManager->SetSetting(node.Section, node.Key, val);
			TheSettingManager->LoadSettings();
		}
		break;
	}
	default: {
		char buf[80];
		strncpy_s(buf, node.Value, sizeof(buf));
		if (ImGui::InputText(node.Key, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
			TheSettingManager->SetSettingS(node.Section, node.Key, buf);
			TheSettingManager->LoadSettings();
		}
		break;
	}
	}

	if (ImGui::IsItemHovered() && !node.Description.empty())
		HoveredDescription = node.Description;

	ImGui::PopID();
}

static void RenderContent() {
	if (SelectedSection.empty()) {
		ImGui::TextDisabled("Select a section from the left panel.");
		return;
	}

	SettingManager::Configuration::SettingList settings;
	TheSettingManager->FillMenuSettings(&settings, SelectedSection.c_str());

	if (settings.empty()) {
		ImGui::TextDisabled("No settings in this section.");
		return;
	}

	HoveredDescription.clear();

	ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "%s", SelectedSection.c_str());
	ImGui::Separator();
	ImGui::Spacing();

	for (auto& setting : settings)
		RenderSetting(setting);

	if (!HoveredDescription.empty()) {
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextWrapped("%s", HoveredDescription.c_str());
	}
}

// Recursive sidebar tree — path is the full dotted section path, name is the display label.
static void RenderSectionNode(const std::string& path, const std::string& name, bool parentIsShaders) {
	StringList subsections;
	TheSettingManager->FillMenuSections(&subsections, path.c_str());

	bool isLeaf = subsections.empty();

	// Scope all widget IDs to this node's unique path.
	ImGui::PushID(path.c_str());

	if (isLeaf) {
		bool selected = (SelectedSection == path);
		if (ImGui::Selectable(name.c_str(), selected))
			SelectedSection = path;
		ImGui::PopID();
		return;
	}

	bool isShaderEntry = parentIsShaders;
	bool open = false;

	if (isShaderEntry) {
		bool enabled = TheSettingManager->GetMenuShaderEnabled(name.c_str());
		bool forced  = TheSettingManager->IsShaderForced(name.c_str());

		ImGui::PushStyleColor(ImGuiCol_Text, enabled
			? ImVec4(0.40f, 1.00f, 0.40f, 1.0f)
			: ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
		open = ImGui::TreeNode("##node");
		ImGui::PopStyleColor();

		ImGui::SameLine();
		ImGui::BeginDisabled(forced);
		if (ImGui::Checkbox(name.c_str(), &enabled)) {
			TheShaderManager->SwitchShaderStatus(name.c_str());
			TheSettingManager->LoadSettings();
		}
		ImGui::EndDisabled();

		if (forced) {
			ImGui::SameLine();
			ImGui::TextDisabled("(forced)");
		}

		// Show render time in debug mode
		if (TheSettingManager->SettingsMain.Develop.DebugMode) {
			EffectRecord* effect = TheShaderManager->GetEffectByName(name.c_str());
			if (effect) {
				float total = max(effect->renderTime + effect->constantUpdateTime, 0.0f);
				if (!TheSettingManager->SettingsMain.Main.RenderEffects) total = 0.0f;
				std::stringstream ss;
				ss << std::fixed << std::setprecision(3) << total << " ms";
				ImGui::SameLine();
				ImGui::TextDisabled("%s", ss.str().c_str());
			}
		}
	} else {
		open = ImGui::TreeNode("##node");
		ImGui::SameLine();
		ImGui::TextUnformatted(name.c_str());
	}

	if (open) {
		bool childrenAreShaders = (path == "Shaders");
		for (auto& sub : subsections)
			RenderSectionNode(path + "." + sub, sub, childrenAreShaders);
		ImGui::TreePop();
	}

	ImGui::PopID();
}

static void RenderSidebar() {
	StringList topSections;
	TheSettingManager->FillMenuSections(&topSections, nullptr);
	for (auto& section : topSections)
		RenderSectionNode(section, section, false);
}

static void RenderMainMenuToast() {
	static auto startTime = std::chrono::system_clock::now();
	auto elapsed = std::chrono::duration<double>(std::chrono::system_clock::now() - startTime).count();
	if (elapsed > 5.0) return;

	std::string msg = std::string(PluginVersion::VersionString)
		+ " loaded  |  press "
		+ TheGameMenuManager->GetKeyName(TheSettingManager->SettingsMain.Menu.KeyEnable)
		+ " to open settings";

	ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	ImVec2 textSize    = ImGui::CalcTextSize(msg.c_str());
	ImVec2 pos         = ImVec2((displaySize.x - textSize.x) * 0.5f, displaySize.y - textSize.y - 12.0f);

	ImGui::GetForegroundDrawList()->AddText(
		ImVec2(pos.x + 1, pos.y + 1),
		IM_COL32(0, 0, 0, 200),
		msg.c_str());
	ImGui::GetForegroundDrawList()->AddText(
		pos,
		IM_COL32(220, 220, 220, 255),
		msg.c_str());
}

void ImGuiManager::BuildUI() {
	// Main menu: show toast only, no settings window
	if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main)) {
		RenderMainMenuToast();
		return;
	}

	if (!Visible) return;

	ImGui::SetNextWindowSize(ImVec2(960.0f, 620.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(500.0f, 300.0f), ImVec2(FLT_MAX, FLT_MAX));

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if (!ImGui::Begin("New Vegas Reloaded", nullptr, flags)) {
		ImGui::End();
		return;
	}

	// Toolbar row
	if (TheSettingManager->hasUnsavedChanges)
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "/!\\ Unsaved changes");
	else
		ImGui::TextDisabled("New Vegas Reloaded  %s", PluginVersion::VersionString);

	{
		const float btnRevert = 54.0f, btnSaveTo = 72.0f, btnSave = 54.0f;
		const float spacing   = ImGui::GetStyle().ItemSpacing.x;
		const float totalW    = btnRevert + btnSaveTo + btnSave + spacing * 2.0f;
		ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - totalW);

		if (ImGui::Button("Revert", ImVec2(btnRevert, 0.0f))) {
			TheSettingManager->RevertSettings();
			SelectedSection.clear();
		}
		ImGui::SameLine();
		if (ImGui::Button("Save to...", ImVec2(btnSaveTo, 0.0f))) {
			char path[MAX_PATH] = "NewVegasReloaded.dll.toml";
			OPENFILENAMEA ofn   = {};
			ofn.lStructSize     = sizeof(ofn);
			ofn.hwndOwner       = GameWindow;
			ofn.lpstrFilter     = "TOML Files\0*.toml\0All Files\0*.*\0";
			ofn.lpstrFile       = path;
			ofn.nMaxFile        = MAX_PATH;
			ofn.lpstrDefExt     = "toml";
			ofn.Flags           = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
			InFileDialog = true;
			bool ok = GetSaveFileNameA(&ofn) != 0;
			InFileDialog = false;
			SetOverlayVisible(true);
			if (ok)
				TheSettingManager->SaveSettingsTo(path);
		}
		ImGui::SameLine();
		if (ImGui::Button("Save", ImVec2(btnSave, 0.0f))) {
			TheSettingManager->SaveSettings();
			InterfaceManager->ShowMessage("Settings saved.");
		}
	}

	ImGui::Separator();

	// Two-panel layout
	float sidebarW = 260.0f;
	float contentH = ImGui::GetContentRegionAvail().y;

	ImGui::BeginChild("##sidebar", ImVec2(sidebarW, contentH), true);
	RenderSidebar();
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##content", ImVec2(0.0f, contentH), true);
	RenderContent();
	ImGui::EndChild();

	ImGui::End();
}
