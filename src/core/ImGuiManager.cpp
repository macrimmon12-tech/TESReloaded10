#include "ImGuiManager.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

// Forward declaration from imgui_impl_win32 — not exposed in the public header
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool    ImGuiManager::Initialized     = false;
bool    ImGuiManager::Visible         = false;
bool    ImGuiManager::DeviceLost      = false;
HWND    ImGuiManager::GameWindow      = nullptr;
WNDPROC ImGuiManager::OriginalWndProc = nullptr;

LRESULT CALLBACK ImGuiManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

	Initialized = true;
	Logger::Log("ImGuiManager: Initialized");
}

void ImGuiManager::NewFrame() {
	if (!Initialized) {
		Initialize();
		if (!Initialized) return;
	}

	// Handle device lost/reset so we never submit draw calls with a dead device
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

	// F11 toggles the overlay as a placeholder; Step 6 replaces this
	// with the configured activation key from SettingManager.
	if (GetAsyncKeyState(VK_F11) & 1)
		Visible = !Visible;

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
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

	SetWindowLong(GameWindow, GWL_WNDPROC, (LONG)(LONG_PTR)OriginalWndProc);

	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	Initialized = false;
	Logger::Log("ImGuiManager: Shutdown");
}
