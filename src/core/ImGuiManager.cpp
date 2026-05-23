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

static float s_pendingWheelDelta    = 0.0f;
static float s_shaderStepSize       = 0.1f;
static bool  s_colorStatesNeedReset = false;

static HRESULT WINAPI HookedGetDeviceState(IDirectInputDevice8* device, DWORD cbData, LPVOID lpvData) {
	HRESULT hr = OriginalGetDeviceState(device, cbData, lpvData);
	if (SUCCEEDED(hr) && cbData == sizeof(DIMOUSESTATE2) && ImGuiManager::IsVisible()) {
		// Capture scroll wheel delta before zeroing — WM_MOUSEWHEEL is suppressed by DXVK.
		s_pendingWheelDelta += ((DIMOUSESTATE2*)lpvData)->lZ / (float)WHEEL_DELTA;
		memset(lpvData, 0, cbData);
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
		PatchMouseVTable();
		ClipCursor(nullptr);
		BlockGameInput(true);
		ImGui::GetIO().MouseDrawCursor = true;
		ImGui::GetIO().ClearInputKeys();
	} else {
		BlockGameInput(false);
		ImGui::GetIO().MouseDrawCursor = false;
	}
}

static void HandleRawKeyboard(const RAWKEYBOARD& kb);

// ---- WndProc -----------------------------------------------------------------

LRESULT CALLBACK ImGuiManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
		SetOverlayVisible(false);

	if (msg == WM_INPUT) {
		RAWINPUT raw = {};
		UINT size = sizeof(raw);
		GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
		if (raw.header.dwType == RIM_TYPEKEYBOARD)
			HandleRawKeyboard(raw.data.keyboard);
		// Fall through so DefWindowProc can clean up the raw input buffer.
	}

	// Eat WM_SYSKEYDOWN for Alt so Windows never activates the system menu.
	if (Visible && msg == WM_SYSKEYDOWN && wParam == VK_MENU)
		return TRUE;

	if (Visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
		return TRUE;
	// Eat scroll messages so the game's WndProc doesn't interfere with ImGui scroll.
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

	// Register for Raw Input keyboard — delivers WM_INPUT even under DXVK where
	// WM_KEYDOWN is suppressed.  No RIDEV_NOLEGACY so legacy WM messages keep
	// flowing (WndProc still needs to eat WM_SYSKEYDOWN for Alt).
	RAWINPUTDEVICE rid = {};
	rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
	rid.usUsage     = 0x06; // HID_USAGE_GENERIC_KEYBOARD
	rid.dwFlags     = 0;
	rid.hwndTarget  = hwnd;
	RegisterRawInputDevices(&rid, 1, sizeof(rid));

	Initialized = true;
	Logger::Log("ImGuiManager: Initialized");
}

void ImGuiManager::Shutdown() {
	if (!Initialized) return;
	SetOverlayVisible(false);
	RAWINPUTDEVICE rid = {};
	rid.usUsagePage = 0x01;
	rid.usUsage     = 0x06;
	rid.dwFlags     = RIDEV_REMOVE;
	rid.hwndTarget  = nullptr;
	RegisterRawInputDevices(&rid, 1, sizeof(rid));
	SetWindowLong(GameWindow, GWL_WNDPROC, (LONG)(LONG_PTR)OriginalWndProc);
	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	Initialized = false;
	Logger::Log("ImGuiManager: Shutdown");
}

// ---- Raw Input keyboard handler ---------------------------------------------
// WM_KEYDOWN is suppressed by DXVK; WM_INPUT is not.  We register for raw
// keyboard input in Initialize() and handle it here.  This gives us correct
// VKeys for ALL keys (including extended keys like END) without MapVirtualKey.

static ImGuiKey VkToImGuiKey(USHORT vk) {
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
	case VK_SPACE:    return ImGuiKey_Space;
	case VK_ADD:      return ImGuiKey_KeypadAdd;
	case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
	case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
	case VK_DIVIDE:   return ImGuiKey_KeypadDivide;
	case VK_NUMPAD0:  return ImGuiKey_Keypad0;
	case VK_NUMPAD1:  return ImGuiKey_Keypad1;
	case VK_NUMPAD2:  return ImGuiKey_Keypad2;
	case VK_NUMPAD3:  return ImGuiKey_Keypad3;
	case VK_NUMPAD4:  return ImGuiKey_Keypad4;
	case VK_NUMPAD5:  return ImGuiKey_Keypad5;
	case VK_NUMPAD6:  return ImGuiKey_Keypad6;
	case VK_NUMPAD7:  return ImGuiKey_Keypad7;
	case VK_NUMPAD8:  return ImGuiKey_Keypad8;
	case VK_NUMPAD9:  return ImGuiKey_Keypad9;
	case 'A': return ImGuiKey_A; case 'B': return ImGuiKey_B;
	case 'C': return ImGuiKey_C; case 'D': return ImGuiKey_D;
	case 'E': return ImGuiKey_E; case 'F': return ImGuiKey_F;
	case 'G': return ImGuiKey_G; case 'H': return ImGuiKey_H;
	case 'I': return ImGuiKey_I; case 'J': return ImGuiKey_J;
	case 'K': return ImGuiKey_K; case 'L': return ImGuiKey_L;
	case 'M': return ImGuiKey_M; case 'N': return ImGuiKey_N;
	case 'O': return ImGuiKey_O; case 'P': return ImGuiKey_P;
	case 'Q': return ImGuiKey_Q; case 'R': return ImGuiKey_R;
	case 'S': return ImGuiKey_S; case 'T': return ImGuiKey_T;
	case 'U': return ImGuiKey_U; case 'V': return ImGuiKey_V;
	case 'W': return ImGuiKey_W; case 'X': return ImGuiKey_X;
	case 'Y': return ImGuiKey_Y; case 'Z': return ImGuiKey_Z;
	case '0': return ImGuiKey_0; case '1': return ImGuiKey_1;
	case '2': return ImGuiKey_2; case '3': return ImGuiKey_3;
	case '4': return ImGuiKey_4; case '5': return ImGuiKey_5;
	case '6': return ImGuiKey_6; case '7': return ImGuiKey_7;
	case '8': return ImGuiKey_8; case '9': return ImGuiKey_9;
	default:  return ImGuiKey_None;
	}
}

// Hardcoded DIK->VK for extended keys where MapVirtualKey returns wrong/zero.
static int DikToVk(BYTE dik) {
	switch (dik) {
	case 0x9C: return VK_RETURN;   // DIK_NUMPADENTER
	case 0x9D: return VK_RCONTROL; // DIK_RCONTROL
	case 0xB5: return VK_DIVIDE;   // DIK_NUMPADSLASH
	case 0xB8: return VK_RMENU;    // DIK_RALT
	case 0xC5: return VK_PAUSE;    // DIK_PAUSE
	case 0xC7: return VK_HOME;     // DIK_HOME
	case 0xC8: return VK_UP;       // DIK_UP
	case 0xC9: return VK_PRIOR;    // DIK_PGUP
	case 0xCB: return VK_LEFT;     // DIK_LEFT
	case 0xCD: return VK_RIGHT;    // DIK_RIGHT
	case 0xCF: return VK_END;      // DIK_END
	case 0xD0: return VK_DOWN;     // DIK_DOWN
	case 0xD1: return VK_NEXT;     // DIK_PGDN
	case 0xD2: return VK_INSERT;   // DIK_INSERT
	case 0xD3: return VK_DELETE;   // DIK_DELETE
	default:   return (int)MapVirtualKey(dik, MAPVK_VSC_TO_VK_EX);
	}
}

static void HandleRawKeyboard(const RAWKEYBOARD& kb) {
	if (kb.VKey == 0 || kb.VKey == 0xFF) return;
	if (!ImGuiManager::IsVisible()) return;

	bool isDown = (kb.Flags & RI_KEY_BREAK) == 0;

	ImGuiIO& io = ImGui::GetIO();

	ImGuiKey imKey = VkToImGuiKey(kb.VKey);
	if (imKey != ImGuiKey_None)
		io.AddKeyEvent(imKey, isDown);

	if (isDown) {
		// Build modifier state from GetAsyncKeyState — accurate under DXVK
		// where WM_KEYDOWN is suppressed and GetKeyboardState may be stale.
		BYTE ks[256] = {};
		auto abit = [](int vk) -> BYTE { return (GetAsyncKeyState(vk) & 0x8000) ? 0x80 : 0; };
		ks[VK_SHIFT]   = abit(VK_SHIFT);
		ks[VK_CONTROL] = abit(VK_CONTROL);
		ks[VK_MENU]    = abit(VK_MENU);
		ks[VK_CAPITAL] = (GetKeyState(VK_CAPITAL) & 1) ? 0x01 : 0;
		WCHAR buf[4] = {};
		int n = ToUnicode(kb.VKey, kb.MakeCode, ks, buf, 4, 0);
		for (int i = 0; i < n; i++)
			io.AddInputCharacterUTF16(buf[i]);
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

	// Toggle overlay via GetAsyncKeyState — reads hardware state directly,
	// works under DXVK regardless of WM message or DI hook availability.
	// DikToVk handles extended keys (END, arrows, etc.) that MapVirtualKey gets wrong.
	if (TheSettingManager) {
		BYTE dik = (BYTE)TheSettingManager->SettingsMain.Menu.KeyEnable;
		int  vk  = DikToVk(dik);
		if (vk) {
			static bool prevToggle = false;
			bool curToggle = (GetAsyncKeyState(vk) & 0x8000) != 0;
			if (curToggle && !prevToggle)
				SetOverlayVisible(!Visible);
			prevToggle = curToggle;
		}
	}

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
