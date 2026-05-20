#include "ImGuiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include <sstream>
#include <iomanip>
#include <ctime>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef HRESULT(WINAPI* GetDeviceState_t)(IDirectInputDevice8*, DWORD, LPVOID);
static GetDeviceState_t OriginalGetDeviceState = nullptr;

bool    ImGuiManager::Initialized     = false;
bool    ImGuiManager::Visible         = false;
bool    ImGuiManager::DeviceLost      = false;
HWND    ImGuiManager::GameWindow      = nullptr;
WNDPROC ImGuiManager::OriginalWndProc = nullptr;

static std::string SelectedSection;

// ---- DirectInput mouse block (zeros lX/lY/lZ deltas + buttons) ---------------
// xNVSE has no public API to block mouse movement deltas, so we patch the
// IDirectInputDevice8 vtable directly.  The hook is installed once and never
// removed; it simply passes through when the overlay is not visible.

static float s_pendingWheelDelta = 0.0f;

static HRESULT WINAPI HookedGetDeviceState(IDirectInputDevice8* device, DWORD cbData, LPVOID lpvData) {
	HRESULT hr = OriginalGetDeviceState(device, cbData, lpvData);
	if (SUCCEEDED(hr) && ImGuiManager::IsVisible()) {
		if (cbData == sizeof(DIMOUSESTATE2)) {
			// Capture scroll wheel delta before zeroing so we can feed it to ImGui.
			// WM_MOUSEWHEEL is suppressed by DXVK so WndProc never sees it.
			s_pendingWheelDelta += ((DIMOUSESTATE2*)lpvData)->lZ / (float)WHEEL_DELTA;
			memset(lpvData, 0, cbData);
		} else if (cbData == 256) {
			// Zero keyboard buffer to block game movement/actions.
			// Preserve the toggle key so OnKeyDown can still close the overlay.
			BYTE toggleKey = TheSettingManager
				? (BYTE)TheSettingManager->SettingsMain.Menu.KeyEnable : 0;
			BYTE toggleState = toggleKey ? ((BYTE*)lpvData)[toggleKey] : 0;
			memset(lpvData, 0, cbData);
			if (toggleKey) ((BYTE*)lpvData)[toggleKey] = toggleState;
		}
	}
	return hr;
}

static void** GetMouseVTable() {
	InputControl* input = Global->GetInputControl();
	if (!input || !input->mouseInterface) return nullptr;
	return *reinterpret_cast<void***>(input->mouseInterface);
}

static void PatchMouseVTable() {
	void** vtable = GetMouseVTable();
	if (!vtable) return;
	if (vtable[9] == reinterpret_cast<void*>(HookedGetDeviceState)) return;
	OriginalGetDeviceState = reinterpret_cast<GetDeviceState_t>(vtable[9]);
	DWORD old;
	VirtualProtect(&vtable[9], sizeof(void*), PAGE_READWRITE, &old);
	vtable[9] = reinterpret_cast<void*>(HookedGetDeviceState);
	VirtualProtect(&vtable[9], sizeof(void*), old, &old);
}

// ---- xNVSE input block (mouse buttons / wheel via DIHookControl) -------------

static void BlockGameInput(bool block) {
	if (g_DIHookCtrl) {
		// Block keyboard (0-255) and mouse buttons/wheel (256+) so the game
		// doesn't process input while the overlay is open.
		for (UInt32 code = 0; code < kMaxMacros; code++)
			g_DIHookCtrl->SetKeyDisableState(code, block, DIHookControl::kDisable_User);
	}
}

// ---- Visibility --------------------------------------------------------------

static void SetOverlayVisible(bool visible) {
	if (ImGuiManager::IsVisible() == visible) return;
	ImGuiManager::SetVisible(visible);
	if (visible) {
		ClipCursor(nullptr);
		BlockGameInput(true);
		ImGui::GetIO().MouseDrawCursor = true;
		ImGui::GetIO().ClearInputKeys();
	} else {
		BlockGameInput(false);
		ImGui::GetIO().MouseDrawCursor = false;
	}
}

// ---- WndProc -----------------------------------------------------------------

LRESULT CALLBACK ImGuiManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
		SetOverlayVisible(false);

	// FNV doesn't call TranslateMessage so WM_CHAR is never posted.
	// Manually convert WM_KEYDOWN to characters when ImGui needs text input.
	// Eat Escape WM so the game never sees it; actual close is handled in BuildUI
	// via polling (waits for key release before unblocking DI).
	if (Visible && msg == WM_KEYDOWN && wParam == VK_ESCAPE)
		return TRUE;

if (Visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
		return TRUE;
	// WndProcHandler returns 0 for scroll messages, so eat them explicitly to
	// prevent the game's WndProc from interfering with ImGui child-window scroll.
	if (Visible && (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL))
		return 0;
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
	PatchMouseVTable();

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

// ---- Input polling (DXVK suppresses WM_KEYDOWN/WM_MOUSEWHEEL) ---------------

static ImGuiKey VkToImGuiKey(int vk) {
	switch (vk) {
	case VK_TAB:      return ImGuiKey_Tab;
	case VK_LEFT:     return ImGuiKey_LeftArrow;
	case VK_RIGHT:    return ImGuiKey_RightArrow;
	case VK_UP:       return ImGuiKey_UpArrow;
	case VK_DOWN:     return ImGuiKey_DownArrow;
	case VK_PRIOR:    return ImGuiKey_PageUp;
	case VK_NEXT:     return ImGuiKey_PageDown;
	case VK_HOME:     return ImGuiKey_Home;
	case VK_END:      return ImGuiKey_End;
	case VK_INSERT:   return ImGuiKey_Insert;
	case VK_DELETE:   return ImGuiKey_Delete;
	case VK_BACK:     return ImGuiKey_Backspace;
	case VK_RETURN:   return ImGuiKey_Enter;
	case VK_ESCAPE:   return ImGuiKey_Escape;
	case VK_LSHIFT:   return ImGuiKey_LeftShift;
	case VK_RSHIFT:   return ImGuiKey_RightShift;
	case VK_LCONTROL: return ImGuiKey_LeftCtrl;
	case VK_RCONTROL: return ImGuiKey_RightCtrl;
	case VK_LMENU:    return ImGuiKey_LeftAlt;
	case VK_RMENU:    return ImGuiKey_RightAlt;
	case 'A': return ImGuiKey_A;
	case 'C': return ImGuiKey_C;
	case 'V': return ImGuiKey_V;
	case 'X': return ImGuiKey_X;
	case 'Z': return ImGuiKey_Z;
	default:          return ImGuiKey_None;
	}
}

static void PollKeyboardForImGui() {
	static bool prevState[256] = {};
	ImGuiIO& io = ImGui::GetIO();

	// Convert the DIK toggle key to a VK so we can skip it — it's for open/close
	// only and should not also navigate ImGui widgets.
	int toggleVK = 0;
	if (TheSettingManager) {
		BYTE dik = (BYTE)TheSettingManager->SettingsMain.Menu.KeyEnable;
		bool ext  = (dik & 0x80) != 0;
		UINT scan = ext ? ((dik & 0x7F) | 0x100) : dik;
		toggleVK = (int)MapVirtualKey(scan, MAPVK_VSC_TO_VK_EX);
	}

	// Build keyboard state for ToUnicode using async values for modifiers.
	BYTE ks[256] = {};
	GetKeyboardState(ks);
	auto abit = [](int vk) -> BYTE { return (GetAsyncKeyState(vk) & 0x8000) ? 0x80 : 0; };
	ks[VK_SHIFT]   = abit(VK_SHIFT);
	ks[VK_CONTROL] = abit(VK_CONTROL);
	ks[VK_MENU]    = abit(VK_MENU);
	ks[VK_CAPITAL] = (GetKeyState(VK_CAPITAL) & 1) ? 0x01 : 0;

	for (int vk = 1; vk < 256; vk++) {
		// Skip generic aliases — we handle left/right variants explicitly.
		if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
		// Skip the overlay toggle key — handled by OnKeyDown, not ImGui nav.
		if (toggleVK && vk == toggleVK) { prevState[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0; continue; }

		bool cur  = (GetAsyncKeyState(vk) & 0x8000) != 0;
		bool prev = prevState[vk];

		// Inject key state transitions for navigation/modifier keys.
		ImGuiKey key = VkToImGuiKey(vk);
		if (key != ImGuiKey_None && cur != prev)
			io.AddKeyEvent(key, cur);

		// Inject printable character on fresh press.
		if (cur && !prev) {
			UINT scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
			WCHAR buf[4] = {};
			int n = ToUnicode(vk, scan, ks, buf, 4, 0);
			for (int i = 0; i < n; i++)
				io.AddInputCharacterUTF16(buf[i]);
		}

		prevState[vk] = cur;
	}
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
		PatchMouseVTable();
	}

	// Close if a game menu is active (inventory, pip-boy, etc.)
	if (Visible && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_None))
		SetOverlayVisible(false);

	// Toggle via DirectInput raw buffer — OnKeyDown reads CurrentKeyState which
	// is the raw DI buffer, preserved for this key even when the rest is zeroed.
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
		if (s_pendingWheelDelta != 0.0f) {
			io.AddMouseWheelEvent(0.0f, s_pendingWheelDelta);
			s_pendingWheelDelta = 0.0f;
		}
		PollKeyboardForImGui();
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
		// A local buf reset from node.Value each frame causes ImGui to detect an
		// external buffer change and reset the in-progress edit every frame.
		// Persist a string per widget ID; only refresh from node.Value when idle.
		static std::unordered_map<ImGuiID, std::string> sBufs;
		ImGuiID id = ImGui::GetID(node.Key);
		std::string& persistent = sBufs[id];
		if (ImGui::GetActiveID() != id)
			persistent = node.Value;
		char buf[80];
		strncpy_s(buf, persistent.c_str(), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		if (ImGui::InputText(node.Key, buf, sizeof(buf)))
			persistent = buf; // keep in sync while ImGui writes back each frame
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			TheSettingManager->SetSettingS(node.Section, node.Key, buf);
			TheSettingManager->LoadSettings();
		}
		break;
	}
	}

	if (ImGui::IsItemHovered() && !node.Description.empty()) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
		ImGui::TextUnformatted(node.Description.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

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

	ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "%s", SelectedSection.c_str());
	ImGui::Separator();
	ImGui::Spacing();

	for (auto& setting : settings)
		RenderSetting(setting);


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

	// Wait for Escape release before closing so the game doesn't see it still held.
	static bool escapePending = false;
	if (ImGui::IsKeyPressed(ImGuiKey_Escape)) escapePending = true;
	if (escapePending && !ImGui::IsKeyDown(ImGuiKey_Escape)) {
		escapePending = false;
		SetOverlayVisible(false);
		return;
	}

	ImGui::SetNextWindowSize(ImVec2(960.0f, 620.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(500.0f, 300.0f), ImVec2(FLT_MAX, FLT_MAX));

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	bool open = true;
	if (!ImGui::Begin("New Vegas Reloaded", &open, flags) || !open) {
		ImGui::End();
		if (!open) SetOverlayVisible(false);
		return;
	}

	// Toolbar row
	if (TheSettingManager->hasUnsavedChanges)
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "/!\\ Unsaved changes");
	else
		ImGui::TextDisabled("New Vegas Reloaded  %s", PluginVersion::VersionString);

	{
		const float btnRevert = 54.0f, btnCopy = 72.0f, btnSave = 54.0f;
		const float spacing   = ImGui::GetStyle().ItemSpacing.x;
		const float totalW    = btnRevert + btnCopy + btnSave + spacing * 2.0f;
		ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - totalW);

		if (ImGui::Button("Revert", ImVec2(btnRevert, 0.0f))) {
			TheSettingManager->RevertSettings();
			SelectedSection.clear();
		}
		ImGui::SameLine();
		if (ImGui::Button("Save Copy", ImVec2(btnCopy, 0.0f))) {
			// Build timestamped path next to the DLL, e.g.:
			// NewVegasReloaded_20260519_2119_WastelandNV.dll.toml
			char dllPath[MAX_PATH] = {};
			HMODULE hMod = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)&ImGuiManager::Initialize, &hMod);
			GetModuleFileNameA(hMod, dllPath, MAX_PATH);

			// Strip ".dll" suffix to get base path
			char* ext = strrchr(dllPath, '.');
			if (ext) *ext = '\0';

			// Timestamp
			time_t now = time(nullptr);
			tm lt = {};
			localtime_s(&lt, &now);
			char ts[32] = {};
			strftime(ts, sizeof(ts), "%Y%m%d_%H%M", &lt);

			// Location: always include worldspace + cell editor ID for TTW compatibility.
			auto sanitize = [](const char* s, std::string& out) {
				if (s && s[0])
					for (const char* c = s; *c; c++)
						out += (isalnum((unsigned char)*c) ? *c : '_');
			};
			std::string loc;
			if (Player && Player->parentCell) {
				TESWorldSpace* ws = Player->GetWorldSpace();
				std::string wsName, cellName;
				if (ws) sanitize(ws->GetEditorName(), wsName);
				sanitize(Player->parentCell->GetEditorName(), cellName);
				if (!wsName.empty()) loc = wsName;
				if (!cellName.empty()) loc += (loc.empty() ? "" : "_") + cellName;
				// Append grid coords for exterior cells
				TESObjectCELL::CellCoordinates* coords = Player->parentCell->coords;
				if (coords) {
					char grid[32] = {};
					_snprintf_s(grid, sizeof(grid), "_%d_%d", coords->x, coords->y);
					loc += grid;
				}
			}

			char savePath[MAX_PATH] = {};
			if (loc.empty())
				_snprintf_s(savePath, sizeof(savePath), "%s_%s.dll.toml", dllPath, ts);
			else
				_snprintf_s(savePath, sizeof(savePath), "%s_%s_%s.dll.toml", dllPath, ts, loc.c_str());

			TheSettingManager->SaveSettingsTo(savePath);
			InterfaceManager->ShowMessage("Settings copy saved.");
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
