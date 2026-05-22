#include "ImGuiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include <unordered_set>

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

static float s_pendingWheelDelta      = 0.0f;
static float s_shaderStepSize         = 0.1f;
static bool  s_colorStatesNeedReset   = false;

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
	// Eat WM_SYSKEYDOWN for Alt so Windows never activates the menu bar.
	if (Visible && msg == WM_SYSKEYDOWN && wParam == VK_MENU)
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

static bool ShouldHideSection(const std::string& name) {
	return name == "WeatherMode";
}

static bool ShouldHideKey(const char* key) {
	if (strncmp(key, "TextColor", 9) == 0 || strncmp(key, "TextShadow", 10) == 0) return true;
	// LUT filenames are rendered as cycle pickers, not raw InputText
	if (strcmp(key, "DayLUT") == 0 || strcmp(key, "NightLUT") == 0 || strcmp(key, "InteriorLUT") == 0) return true;
	return false;
}

static const struct { int dik; const char* name; } kDIKTable[] = {
	{ 0x01, "Escape" },
	{ 0x02, "1" }, { 0x03, "2" }, { 0x04, "3" }, { 0x05, "4" }, { 0x06, "5" },
	{ 0x07, "6" }, { 0x08, "7" }, { 0x09, "8" }, { 0x0A, "9" }, { 0x0B, "0" },
	{ 0x0C, "Minus (-)" }, { 0x0D, "Equals (=)" }, { 0x0E, "Backspace" },
	{ 0x0F, "Tab" },
	{ 0x10, "Q" }, { 0x11, "W" }, { 0x12, "E" }, { 0x13, "R" }, { 0x14, "T" },
	{ 0x15, "Y" }, { 0x16, "U" }, { 0x17, "I" }, { 0x18, "O" }, { 0x19, "P" },
	{ 0x1A, "[ Left Bracket" }, { 0x1B, "] Right Bracket" }, { 0x1C, "Enter" },
	{ 0x1D, "Left Ctrl" },
	{ 0x1E, "A" }, { 0x1F, "S" }, { 0x20, "D" }, { 0x21, "F" }, { 0x22, "G" },
	{ 0x23, "H" }, { 0x24, "J" }, { 0x25, "K" }, { 0x26, "L" },
	{ 0x27, "Semicolon (;)" }, { 0x28, "Apostrophe (')" }, { 0x29, "Grave (`)" },
	{ 0x2A, "Left Shift" }, { 0x2B, "Backslash (\\)" },
	{ 0x2C, "Z" }, { 0x2D, "X" }, { 0x2E, "C" }, { 0x2F, "V" },
	{ 0x30, "B" }, { 0x31, "N" }, { 0x32, "M" },
	{ 0x33, "Comma (,)" }, { 0x34, "Period (.)" }, { 0x35, "Slash (/)" },
	{ 0x36, "Right Shift" }, { 0x37, "Numpad *" }, { 0x38, "Left Alt" },
	{ 0x39, "Space" }, { 0x3A, "Caps Lock" },
	{ 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" },
	{ 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" }, { 0x42, "F8" },
	{ 0x43, "F9" }, { 0x44, "F10" },
	{ 0x45, "Num Lock" }, { 0x46, "Scroll Lock" },
	{ 0x47, "Numpad 7" }, { 0x48, "Numpad 8" }, { 0x49, "Numpad 9" },
	{ 0x4A, "Numpad -" },
	{ 0x4B, "Numpad 4" }, { 0x4C, "Numpad 5" }, { 0x4D, "Numpad 6" },
	{ 0x4E, "Numpad +" },
	{ 0x4F, "Numpad 1" }, { 0x50, "Numpad 2" }, { 0x51, "Numpad 3" },
	{ 0x52, "Numpad 0" }, { 0x53, "Numpad ." },
	{ 0x57, "F11" }, { 0x58, "F12" },
	{ 0x9C, "Numpad Enter" }, { 0x9D, "Right Ctrl" },
	{ 0xB5, "Numpad /" }, { 0xB7, "Print Screen" }, { 0xB8, "Right Alt" },
	{ 0xC5, "Pause" }, { 0xC7, "Home" },
	{ 0xC8, "Up Arrow" }, { 0xC9, "Page Up" }, { 0xCB, "Left Arrow" },
	{ 0xCD, "Right Arrow" }, { 0xCF, "End" },
	{ 0xD0, "Down Arrow" }, { 0xD1, "Page Down" },
	{ 0xD2, "Insert" }, { 0xD3, "Delete" },
};

static void RenderDIKPopup() {
	if (!ImGui::BeginPopup("DIKReference")) return;
	ImGui::Text("DirectInput Scancodes");
	ImGui::Separator();
	if (ImGui::BeginTable("diktbl", 2,
		ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
		ImVec2(220.0f, 320.0f)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Code");
		ImGui::TableSetupColumn("Key");
		ImGui::TableHeadersRow();
		for (auto& e : kDIKTable) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::Text("0x%02X (%d)", e.dik, e.dik);
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(e.name);
		}
		ImGui::EndTable();
	}
	ImGui::EndPopup();
}

static void RenderColorTriple(
	SettingManager::Configuration::ConfigNode& nodeR,
	SettingManager::Configuration::ConfigNode& nodeG,
	SettingManager::Configuration::ConfigNode& nodeB,
	const std::string& prefix)
{
	// Persistent state: only initialise from node values once per section visit.
	// Re-reading each frame forces max(col)=1 every frame, pinning the picker
	// dot to the top edge of the triangle and resetting the intensity slider.
	struct ColorState { float col[3]; float scale; };
	static std::unordered_map<std::string, ColorState> s_states;

	if (s_colorStatesNeedReset) {
		s_states.clear();
		s_colorStatesNeedReset = false;
	}

	std::string stateKey = std::string(nodeR.Section) + "." + prefix;
	if (s_states.find(stateKey) == s_states.end()) {
		float rv = (float)atof(nodeR.Value);
		float gv = (float)atof(nodeG.Value);
		float bv = (float)atof(nodeB.Value);
		float sc = rv > gv ? rv : gv;
		if (bv > sc) sc = bv;
		ColorState cs;
		if (sc > 0.0f) {
			cs.scale    = sc;
			cs.col[0]   = rv / sc;
			cs.col[1]   = gv / sc;
			cs.col[2]   = bv / sc;
		} else {
			cs.scale    = 1.0f;
			cs.col[0]   = cs.col[1] = cs.col[2] = 0.0f;
		}
		s_states[stateKey] = cs;
	}
	ColorState& cs = s_states[stateKey];

	// Trim trailing underscores for display
	std::string label = prefix;
	while (!label.empty() && label.back() == '_') label.pop_back();

	ImGui::PushID(prefix.c_str());
	ImGui::TextUnformatted(label.c_str());

	bool changed = false;
	if (ImGui::ColorPicker3("##col", cs.col,
		ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview))
		changed = true;

	ImGui::Text("Intensity");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	if (ImGui::DragFloat("##intensity", &cs.scale, 0.01f, 0.0f, 0.0f, "%.4f"))
		changed = true;

	if (changed) {
		TheSettingManager->SetSetting(nodeR.Section, nodeR.Key, cs.col[0] * cs.scale);
		TheSettingManager->SetSetting(nodeG.Section, nodeG.Key, cs.col[1] * cs.scale);
		TheSettingManager->SetSetting(nodeB.Section, nodeB.Key, cs.col[2] * cs.scale);
		TheSettingManager->LoadSettings();
	}

	if (!nodeR.Description.empty()) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
		ImGui::TextUnformatted(nodeR.Description.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	ImGui::PopID();
}

static void RenderSetting(SettingManager::Configuration::ConfigNode& node, bool isShader = false) {
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
		bool hovered = ImGui::IsItemHovered();
		if (isShader) {
			ImGui::SameLine();
			if (ImGui::SmallButton("-")) {
				val -= s_shaderStepSize;
				TheSettingManager->SetSetting(node.Section, node.Key, val);
				TheSettingManager->LoadSettings();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("+")) {
				val += s_shaderStepSize;
				TheSettingManager->SetSetting(node.Section, node.Key, val);
				TheSettingManager->LoadSettings();
			}
		}
		if (hovered && !node.Description.empty()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
			ImGui::TextUnformatted(node.Description.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		ImGui::PopID();
		return;
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
		bool hovered = ImGui::IsItemHovered();
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			TheSettingManager->SetSettingS(node.Section, node.Key, buf);
			TheSettingManager->LoadSettings();
		}
		if (strcmp(node.Key, "KeyEnable") == 0) {
			ImGui::SameLine();
			if (ImGui::SmallButton("(?)"))
				ImGui::OpenPopup("DIKReference");
			RenderDIKPopup();
		}
		if (hovered && !node.Description.empty()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
			ImGui::TextUnformatted(node.Description.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		ImGui::PopID();
		return;
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

	bool isShader = (SelectedSection.find("Shaders") == 0);

	ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "%s", SelectedSection.c_str());
	ImGui::Separator();
	ImGui::Spacing();

	// LUT section: render DNI cycle pickers before the normal settings loop
	if (SelectedSection == "Shaders.LUT.Main") {
		LUTEffect* lut = TheShaderManager->Effects.LUT;
		if (lut) {
			if (lut->LUTFiles.empty()) {
				ImGui::TextDisabled("No LUTs found in Data/Textures/NewVegasReloaded/LUTs/");
			} else {
				auto renderPicker = [&](const char* label, int& idx, int slot) {
					ImGui::PushID(label);
					ImGui::Text("%s", label);
					ImGui::SameLine();
					if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) {
						idx = ((idx - 1) + (int)lut->LUTFiles.size()) % (int)lut->LUTFiles.size();
						lut->LoadLUT(slot, lut->LUTFiles[idx].c_str());
					}
					ImGui::SameLine();
					ImGui::TextUnformatted(lut->LUTFiles[idx].c_str());
					ImGui::SameLine();
					if (ImGui::ArrowButton("##next", ImGuiDir_Right)) {
						idx = (idx + 1) % (int)lut->LUTFiles.size();
						lut->LoadLUT(slot, lut->LUTFiles[idx].c_str());
					}
					ImGui::PopID();
				};
				renderPicker("Day     ", lut->DayIdx,      0);
				renderPicker("Night   ", lut->NightIdx,    1);
				renderPicker("Interior", lut->InteriorIdx, 2);
			}
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
		}
	}

	// Build key->index map for RGB triple detection
	std::unordered_map<std::string, int> keyIdx;
	for (int i = 0; i < (int)settings.size(); i++)
		keyIdx[settings[i].Key] = i;

	std::unordered_set<std::string> handled;

	for (auto& s : settings) {
		std::string key(s.Key);
		if (handled.count(key)) continue;
		if (ShouldHideKey(s.Key)) continue;

		// RGB triple → color picker (shader sections only)
		if (isShader && key.size() > 1 && key.back() == 'R') {
			std::string pfx = key.substr(0, key.size() - 1);
			std::string kG = pfx + "G", kB = pfx + "B";
			if (keyIdx.count(kG) && keyIdx.count(kB)) {
				handled.insert(key);
				handled.insert(kG);
				handled.insert(kB);
				RenderColorTriple(s, settings[keyIdx[kG]], settings[keyIdx[kB]], pfx);
				continue;
			}
		}

		RenderSetting(s, isShader);
	}
}

// Recursive sidebar tree — path is the full dotted section path, name is the display label.
static void RenderSectionNode(const std::string& path, const std::string& name, bool parentIsShaders) {
	if (ShouldHideSection(name)) return;

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

	// Wait for Escape or Alt release before closing so the game doesn't see them held.
	static bool escapePending = false;
	static bool altPending    = false;
	if (ImGui::IsKeyPressed(ImGuiKey_Escape))   escapePending = true;
	if (ImGui::IsKeyPressed(ImGuiKey_LeftAlt) ||
	    ImGui::IsKeyPressed(ImGuiKey_RightAlt))  altPending    = true;
	if (escapePending && !ImGui::IsKeyDown(ImGuiKey_Escape)) {
		escapePending = false;
		SetOverlayVisible(false);
		return;
	}
	if (altPending &&
	    !ImGui::IsKeyDown(ImGuiKey_LeftAlt) &&
	    !ImGui::IsKeyDown(ImGuiKey_RightAlt)) {
		altPending = false;
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
			s_colorStatesNeedReset = true;
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

	// Step size selector — always visible, applies to shader +/- buttons
	{
		ImGui::Text("Step:");
		static const float kSteps[]  = { 0.001f, 0.01f, 0.1f, 1.0f };
		static const char* kLabels[] = { "0.001", "0.01", "0.1", "1.0" };
		for (int i = 0; i < 4; i++) {
			ImGui::SameLine();
			bool active = fabsf(s_shaderStepSize - kSteps[i]) < 1e-6f;
			if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
			if (ImGui::SmallButton(kLabels[i])) s_shaderStepSize = kSteps[i];
			if (active) ImGui::PopStyleColor();
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
