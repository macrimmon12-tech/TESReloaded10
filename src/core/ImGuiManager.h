#pragma once

class ImGuiManager {
public:
	static void		Initialize();
	static void		NewFrame();
	static void		Render();
	static void		Shutdown();

	static bool		IsVisible() { return Visible; }
	static void		SetVisible(bool visible) { Visible = visible; }

private:
	static bool		Initialized;
	static bool		Visible;
	static bool		DeviceLost;
	static HWND		GameWindow;
	static WNDPROC	OriginalWndProc;

	static LRESULT CALLBACK	WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static void				BuildUI();
};
