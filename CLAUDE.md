# TESReloaded10 — Dev Notes for Claude

## Project
New Vegas Reloaded (NVR) — a post-process graphics injector for Fallout: New Vegas.
Built as a 32-bit DLL (NewVegasReloaded.vcxproj), injected via xNVSE.

## Key architecture
- `src/core/ImGuiManager.cpp` — ImGui settings overlay (DX9 + Win32 backend)
- `src/core/ShaderManager.cpp` — main render loop; calls `ImGuiManager::NewFrame()` and `Render()`
- `src/core/SettingManager.cpp` — TOML-backed settings; ImGui menu is auto-populated from this
- `lib/imgui` — git submodule, docking branch (`git submodule update --init --recursive` required after fresh clone)
- xNVSE headers live under `src/NewVegas/nvse/`

## DXVK input — critical finding
FNV is commonly run under **DXVK** (Vulkan translation layer). DXVK suppresses
`WM_KEYDOWN` and `WM_MOUSEWHEEL` before they reach any subclassed WndProc.
**Do not rely on WM messages for keyboard or scroll wheel input.**

### What works instead
| Input | Solution |
|---|---|
| Keyboard characters | `GetAsyncKeyState` poll each frame → `ToUnicode` → `io.AddInputCharacterUTF16` |
| Keyboard nav keys (arrows, backspace, etc.) | `GetAsyncKeyState` poll → `io.AddKeyEvent(ImGuiKey_X, state)` |
| Scroll wheel | Read `DIMOUSESTATE2::lZ` from the DI mouse buffer *before* zeroing it in `HookedGetDeviceState`, accumulate in `s_pendingWheelDelta`, inject via `io.AddMouseWheelEvent` in `NewFrame` |
| Mouse buttons | `GetAsyncKeyState(VK_LBUTTON/RBUTTON/MBUTTON)` → `io.AddMouseButtonEvent` each frame |
| Toggle key open/close | `OnKeyDown(KeyEnable)` reads raw DI `CurrentKeyState` buffer — not affected by our zeroing because we preserve that one byte |

### What does NOT work under DXVK
- `ImGui_ImplWin32_WndProcHandler` for keyboard/scroll (WM messages never arrive)
- `WM_KEYDOWN` + `ToUnicode` in WndProc
- Any WM_MOUSEWHEEL handler in WndProc

WM_ACTIVATE and other non-input system messages still arrive normally.

## Input blocking when overlay is open
- **Mouse movement**: vtable hook on `IDirectInputDevice8::GetDeviceState` (slot 9), zeros `DIMOUSESTATE2` buffer. Keyboard device shares the same vtable in dinput8.dll so one patch covers both.
- **Keyboard actions**: same hook zeros the 256-byte keyboard buffer (except toggle key byte).
- **Mouse buttons / wheel macros**: `DIHookControl::SetKeyDisableState` via xNVSE for codes 0–265.

## xNVSE
Always check xNVSE (`src/NewVegas/nvse/`) before writing custom hooks for anything that interacts directly with the game (input, UI, game objects). Prefer xNVSE APIs over vtable patches where available.
