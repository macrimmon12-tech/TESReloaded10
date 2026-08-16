# TESReloaded10 — ImGui / UI Refactor Plan

## Background

This plan consolidates feedback from code review and developer discussion identifying structural problems in the ImGui settings overlay and the settings type system. The goal is to fix active bugs, clean up the settings backend, and improve the usability of the overlay UI — without attempting to replicate ReShade-style custom shader infrastructure. New shader authors are expected to touch C++; this plan does not change that.

---

## Completed Work

### BF-1 — Pointer Cast UB in SettingManager ✓ DONE
**File:** `src/core/SettingManager.cpp`

`(bool*)GetSettingI(...)` was casting an `int` return value to a `bool*` pointer instead of `bool`. Fixed at both call sites.

### BF-2 — Uncached Water Texture in OcclusionManager ✓ DONE
**File:** `src/core/OcclusionManager.cpp`

Direct `D3DXCreateTextureFromFileA` call was bypassing `TextureManager` entirely — no caching. Now routes through `TheTextureManager->GetFileTexture()`.

Note: this texture is a debug-only occlusion map visualization and is never player-visible. The fix is for caching consistency only.

### R1 — Typed Settings Storage ✓ DONE
**File:** `src/core/SettingManager.h/.cpp`

`ConfigNode` now carries `BoolValue`, `IntValue`, and `FloatValue` typed members alongside the raw `Value` string. The `(bool)atof(...)` and similar defensive double-casts are eliminated. Bool and int values no longer silently route through float code paths.

### R2 — Texture Loading Consolidation ✓ DONE
**File:** `src/effects/LUT.cpp`, `src/core/OcclusionManager.cpp`

`LoadLUT()` was bundling texture load + slot assignment + settings persistence in one function. Dissolved into three owners:
1. **Load** → `TextureManager::GetFileTexture()` (canonical, cached)
2. **Assign slot** → `LUTEffect::AssignLUTSlot()`
3. **Persist** → `LUTEffect::SaveLUTSetting()`

`GetFileTexture()` is now the single file texture loading path. All callers route through it.

---

## UI-0 — ImGui Theme / Visual Styling

**File:** `src/core/ImGuiManager.cpp` (style setup block, ~line 738)

The overlay currently applies only `StyleColorsDark()` and three rounding values. A proper NVR-specific theme should be applied — colors, spacing, padding, and font configuration — to give the overlay a consistent identity rather than stock Dear ImGui dark.

**Workflow:** Theme is authored in [ImThemes](https://github.com/Patitotective/ImThemes) and exported as a C++ `ImGui::GetStyle()` block. Paste the exported code into the session and the implementation session will slot it in, replacing the existing style setup.

**Scope:**
- Replace `StyleColorsDark()` call and the three rounding values with the ImThemes-exported style block
- Preserve the existing `FontGlobalScale` / `TextSize` restore logic — do not overwrite scale
- No other changes needed; style setup is self-contained

This is self-contained and has no dependencies.

---

## UI-1 — Color Picker Layout Rework

**File:** `src/core/ImGuiManager.cpp`, `RenderColorTriple()` (~line 1099)

Two problems to fix together:

**Bug — R label on wrong line:** The R channel `DragFloat` uses `nodeR.Key` (no `##` prefix) as its ImGui ID, causing the key name to render as a visible label inline on the header row (alongside the section label and revert `~` button). G and B render on their own lines via the outer loop, making R visually inconsistent.

**Layout — horizontal R/G/B value boxes:** The three channel inputs should be displayed as a compact horizontal row of numeric fields, matching ReShade's color picker layout. Currently R, G, B are stacked vertically as separate `DragFloat` rows. The target is all three inline on one row — `R [ 0.800 ]  G [ 0.600 ]  B [ 0.400 ]` — with the color wheel above.

**Fix:**
- Replace the three separate vertical `DragFloat` calls with a single row using `SameLine()` between `##r`, `##g`, `##b` fields, each sized to one-third of available width
- Use `##r` / `##g` / `##b` IDs throughout — no visible key-name labels on the input fields
- Label each field with a short colored or plain prefix (`R`, `G`, `B`) via `Text()` + `SameLine()` before each input
- Color wheel remains above the input row, unchanged

---

## UI-2 — Enum Dropdowns for Integer Mode Settings

**File:** `src/core/ImGuiManager.cpp`, `RenderSetting()` integer branch (~line 1290)

Currently only `TonemappingMode` has a special-case combo box. Several other integer settings are actually fixed enums and should show labeled dropdowns rather than bare `DragInt`:

| Setting | Values |
|---|---|
| `Cinema.Mode` | 0: Always, 1: Not during dialog, 2: Only during dialog |
| `DOF.*.Mode` | 0: Always, 1: Not during dialog, 2: Only in dialog |
| `SleepingMode.Mode` | 0: … (values TBD from TOML comments) |
| `Shadows.Mode` | 0: VSM, 1: EVSM2, 2: EVSM4 |

**Approach:** Replace the single `TonemappingMode` special-case with a small lookup table mapping `"SectionKey.Key"` → `{label array, count}`. Adding a new enum is then a one-line table entry, not a new `strcmp` branch.

---

## Dependency Order

```
BF-1  ✓ DONE
BF-2  ✓ DONE
R1    ✓ DONE
R2    ✓ DONE

UI-0  Theme / visual styling     ── self-contained, no dependencies
UI-1  Color picker label bug     ── self-contained, no dependencies
UI-2  Enum dropdowns             ── self-contained, no dependencies
```
