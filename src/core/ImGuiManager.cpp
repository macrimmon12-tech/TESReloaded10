#include "ImGuiManager.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Original GetDeviceState from the mouse DirectInput device vtable (index 9)
typedef HRESULT(WINAPI* GetDeviceState_t)(IDirectInputDevice8*, DWORD, LPVOID);
static GetDeviceState_t OriginalGetDeviceState = nullptr;

bool    ImGuiManager::Initialized     = false;
bool    ImGuiManager::Visible         = false;
bool    ImGuiManager::DeviceLost      = false;
HWND    ImGuiManager::GameWindow      = nullptr;
WNDPROC ImGuiManager::OriginalWndProc = nullptr;

// Intercepts the game's mouse DirectInput read. When the overlay is open,
// zero the result so camera movement and button presses don't reach the game.
static HRESULT WINAPI HookedGetDeviceState(IDirectInputDevice8* device, DWORD cbData, LPVOID lpvData) {
	HRESULT hr = OriginalGetDeviceState(device, cbData, lpvData);
	if (SUCCEEDED(hr) && ImGuiManager::IsVisible())
		memset(lpvData, 0, cbData);
	return hr;
}

static void PatchMouseVTable() {
	InputControl* input = Global->GetInputControl();
	if (!input || !input->mouseInterface) return;

	void** vtable = *reinterpret_cast<void***>(input->mouseInterface);
	OriginalGetDeviceState = reinterpret_cast<GetDeviceState_t>(vtable[9]);

	DWORD oldProtect;
	VirtualProtect(&vtable[9], sizeof(void*), PAGE_READWRITE, &oldProtect);
	vtable[9] = reinterpret_cast<void*>(HookedGetDeviceState);
	VirtualProtect(&vtable[9], sizeof(void*), oldProtect, &oldProtect);
}

static void RestoreMouseVTable() {
	if (!OriginalGetDeviceState) return;

	InputControl* input = Global->GetInputControl();
	if (!input || !input->mouseInterface) return;

	void** vtable = *reinterpret_cast<void***>(input->mouseInterface);

	DWORD oldProtect;
	VirtualProtect(&vtable[9], sizeof(void*), PAGE_READWRITE, &oldProtect);
	vtable[9] = reinterpret_cast<void*>(OriginalGetDeviceState);
	VirtualProtect(&vtable[9], sizeof(void*), oldProtect, &oldProtect);
}

static void SetOverlayVisible(bool visible) {
	if (ImGuiManager::IsVisible() == visible) return;
	ImGuiManager::SetVisible(visible);
	if (visible) {
		while (ShowCursor(TRUE) < 0) {}
		ClipCursor(nullptr);
	} else {
		while (ShowCursor(FALSE) >= 0) {}
	}
}

LRESULT CALLBACK ImGuiManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
		SetOverlayVisible(false);

	if (Visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
		return TRUE;
	return CallWindowProc(OriginalWndProc, hwnd, msg, wParam, lParam);
}

void ImGuiManager::Initialize() {
	if (Initialized) return;

	IDirect3DDevice9* device = TheRenderManager->device;
	HWND hwnd = TheRenderManager->m_kWndFocus;

	if (!device || !hwnd) {
		Logger::Log("ImGuiManager: device or window not ready, deferring init");
		return;
	}

	GameWindow = hwnd;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.IniFilename = nullptr;

	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX9_Init(device);

	OriginalWndProc = (WNDPROC)SetWindowLong(hwnd, GWL_WNDPROC, (LONG)(LONG_PTR)WndProc);

	PatchMouseVTable();

	Initialized = true;
	Logger::Log("ImGuiManager: Initialized");
}

void ImGuiManager::NewFrame() {
	if (!Initialized) {
		Initialize();
		if (!Initialized) return;
	}

	HRESULT coop = TheRenderManager->device->TestCooperativeLevel();
	if (coop == D3DERR_DEVICELOST) {
		if (!DeviceLost) {
			ImGui_ImplDX9_InvalidateDeviceObjects();
			DeviceLost = true;
		}
		return;
	}
	if (DeviceLost && coop == D3DERR_DEVICENOTRESET) {
		ImGui_ImplDX9_CreateDeviceObjects();
		DeviceLost = false;
	}

	if (GetAsyncKeyState(VK_F11) & 1)
		SetOverlayVisible(!Visible);

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// WM_LBUTTONDOWN doesn't reliably reach WndProc in fullscreen/captured mode,
	// so feed button state directly from the hardware key table instead.
	if (Visible) {
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
		io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
		io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
	}
}

void ImGuiManager::Render() {
	if (!Initialized || DeviceLost) return;

	if (Visible)
		BuildUI();

	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void ImGuiManager::BuildUI() {
	// Placeholder — replaced in Step 6 with the full NVR settings menu
	ImGui::ShowDemoWindow();
}

void ImGuiManager::Shutdown() {
	if (!Initialized) return;

	SetOverlayVisible(false);
	RestoreMouseVTable();
	SetWindowLong(GameWindow, GWL_WNDPROC, (LONG)(LONG_PTR)OriginalWndProc);

	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	Initialized = false;
	Logger::Log("ImGuiManager: Shutdown");
}
