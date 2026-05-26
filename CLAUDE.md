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

`WM_INPUT` (Raw Input) is also unreliable — DXVK's message pump may not dispatch
`WM_INPUT` to the subclassed WndProc at all. **Do not use WM_INPUT for any keyboard input.**

### What works instead
| Input | Solution |
|---|---|
| Keyboard (all — text, nav, characters) | `GetAsyncKeyState` polled for VK 1–255 in `NewFrame`, edge-detect against `s_prevKeyState`, inject via `io.AddKeyEvent` + `ToUnicode` → `io.AddInputCharacterUTF16` |
| Scroll wheel | Read `DIMOUSESTATE2::lZ` from the DI mouse buffer *before* zeroing it in `HookedGetDeviceState`, accumulate in `s_pendingWheelDelta`, inject via `io.AddMouseWheelEvent` in `NewFrame` |
| Mouse buttons | `GetAsyncKeyState(VK_LBUTTON/RBUTTON/MBUTTON)` → `io.AddMouseButtonEvent` each frame |
| Toggle key open/close | `GetAsyncKeyState(DikToVk(KeyEnable))` polled in `NewFrame` every render frame. `DikToVk` has a hardcoded table for extended keys that `MapVirtualKey` gets wrong (END, HOME, arrows, etc.) |

**Rule**: everything goes through `GetAsyncKeyState` polled on the render thread. No WM messages, no cross-thread queues.

### DIK → VK conversion
`MapVirtualKey(dik, MAPVK_VSC_TO_VK_EX)` **fails silently** for E0-prefixed extended keys
(END=0xCF, HOME=0xC7, arrow keys, Insert, Delete, etc.) — returns 0 or wrong VK.
Use the hardcoded `DikToVk()` table in `ImGuiManager.cpp` instead. Non-extended keys
fall through to `MapVirtualKey` which handles them correctly.

### ImGui keyboard navigation
`ImGuiConfigFlags_NavEnableKeyboard` is intentionally **not set**. With it enabled,
END/HOME/arrow keys leak into ImGui widget navigation (jumping to last item, etc.)
even while the overlay is trying to close. Text field editing is unaffected — it uses
a separate code path that does not require nav to be enabled.

### What does NOT work under DXVK
- `ImGui_ImplWin32_WndProcHandler` for keyboard/scroll (WM messages never arrive)
- `WM_KEYDOWN` + `ToUnicode` in WndProc
- Any `WM_MOUSEWHEEL` handler in WndProc
- `WM_INPUT` for keyboard input (may never be dispatched to WndProc under DXVK)
- `MapVirtualKey` for extended DIK scancodes (returns 0 or wrong VK)

WM_ACTIVATE and other non-input system messages still arrive normally.

## Mouse cursor position — click-to-refocus quirk
When the user **clicks** the game window to refocus (rather than Alt+Tab), the OS
generates a `WM_MOUSEMOVE`. The ImGui Win32 backend handles this by calling
`TrackMouseEvent`, which sets its internal `bd->MouseTrackedArea = 1`. Once that
flag is set, `ImGui_ImplWin32_UpdateMouseData` **suppresses** its `GetCursorPos`
fallback path. No further `WM_MOUSEMOVE` arrives because the game registers raw
input with `RIDEV_NOLEGACY`, which eats legacy mouse messages. Result: cursor
position freezes wherever it was when the click landed.

**Fix** (`NewFrame`, after `ImGui_ImplWin32_NewFrame`): inject our own
`io.AddMousePosEvent` from `GetCursorPos` + `ScreenToClient`. Last write wins in
ImGui's event queue, so it overrides whatever stale position `UpdateMouseData` left.
A bounds-check ensures we only inject when the cursor is inside the client rect.

Multi-monitor alt-tab fix is complementary: `SetOverlayVisible(true)` warps the OS
cursor to the window center via `SetCursorPos` if it is outside the client rect.
This runs once on open; the per-frame injection above keeps it correct thereafter.

## Input blocking when overlay is open
- **Mouse movement**: vtable hook on `IDirectInputDevice8::GetDeviceState` (slot 9), zeros `DIMOUSESTATE2` buffer. Mouse device only — keyboard device has a separate vtable under DXVK.
- **Keyboard actions**: `DIHookControl::SetKeyDisableState` via xNVSE for codes 0–265 (`BlockGameInput`).
- **Mouse buttons / wheel macros**: same `DIHookControl` call covers codes 256+.

## xNVSE
Always check xNVSE (`src/NewVegas/nvse/`) before writing custom hooks for anything that interacts directly with the game (input, UI, game objects). Prefer xNVSE APIs over vtable patches where available.
