# TESReloaded10 — ImGui / Effect Architecture Refactor Plan

## Background

This plan consolidates feedback from code review and developer discussion identifying structural problems in the ImGui settings overlay, the settings type system, and the effect/shader pipeline. The goal is to eliminate spaghetti rendering code, fix active bugs, and establish an architecture that supports a drop-in custom effects system without requiring engine changes for each new effect.

---

## Bug Fixes (Ship Immediately, Independent)

### BF-1 — Pointer Cast UB in SettingManager
**File:** `src/core/SettingManager.cpp:1223` and `:1243`

`(bool*)GetSettingI(...)` casts an `int` return value to a `bool*` pointer instead of `bool`. This is undefined behavior on every call path that hits these functions.

- Line 1223: `bool enabled = (bool*)GetSettingI(settingString, "Enabled");`
- Line 1243: `return (bool*)GetSettingI(settingString, Name);`

**Fix:** Change `(bool*)` to `(bool)` at both sites.

### BF-2 — Uncached Water Texture in OcclusionManager
**File:** `src/core/OcclusionManager.cpp:226`

Direct `D3DXCreateTextureFromFileA` call with a hardcoded path bypasses `TextureManager` entirely — no caching, potential reload on every render pass.

**Fix:** Replace with `TheTextureManager->GetFileTexture(".\\Data\\Textures\\Water\\water00.dds", TextureRecord::PlanarBuffer)`.

---

## Refactor 1 — Type System (Settings Storage)

**Root cause documented at:** `src/core/SettingManager.cpp:56`

All settings are stored as strings internally and re-parsed on every access. This forces every consumer to hand-parse types, causes bool and int values to silently route through float code paths, and requires defensive double-casts such as `(bool)atof(Node.Value)` throughout `SettingManager.cpp` and `ImGuiManager.cpp`.

**Target state:**
- Settings carry a proper typed value (`bool`, `int`, `float`, `string`) rather than raw strings
- `SetSetting` has correctly typed overloads — no implicit bool → float promotion
- Consumers read typed values directly; no `strcmp(value, "1")` or `atof` at call sites
- The defaults TOML is demoted to **default values only** — not a runtime schema driving UI behavior

---

## Refactor 2 — Texture Loading Consolidation

**Canonical loader:** `src/core/TextureManager.cpp:90–115` (`GetFileTexture()`)

Three codepaths currently exist where one should:

| Location | Status |
|---|---|
| `TextureManager::GetFileTexture()` | Canonical — keep and expand as needed |
| `LUT.cpp:21–50` (`LoadLUT`) | Bundles load + slot assignment + settings persistence; should dissolve |
| `OcclusionManager.cpp:226` | Raw D3DX call, no cache — covered by BF-2 |
| `BinkManager`, `Memory.cpp` | In-memory / procedural loaders — legitimately separate, leave alone |

`LoadLUT()` mixes three concerns that should be separated:
1. **Load the texture** → `TextureManager::GetFileTexture()` (canonical)
2. **Assign to the correct slot** (day/night/interior) → `LUTEffect` owns this
3. **Persist the choice to settings** → `LUTEffect` / `EffectRecord` responsibility

`LoadLUT()` dissolves into these three owners. `GetFileTexture()` is the single file texture loading path; all callers route through it.

---

## Refactor 3 — EffectRecord Owns Its Own UI (Core Architecture)

**Current problem concentrated in:** `src/core/ImGuiManager.cpp:1099–1487` (~390 lines, 8+ mixed concerns)

The menu currently holds all effect-specific knowledge. `RenderContent()`, `RenderSetting()`, and `RenderColorTriple()` mix display logic, input handling, tooltip rendering, settings persistence, special-case widget branches, and hardcoded field name checks (`strcmp(node.Key, "TonemappingMode")`, `strcmp(node.Key, "KeyEnable")`) in a single call chain. Every new effect with non-standard UI requires modifying `ImGuiManager`.

### Target Architecture

```
EffectRecord (base class)
  virtual RenderMenu(ImGuiContext)
    └─ default impl: iterate settings, call appropriate ImGui helper per widget hint

  LUTEffect : EffectRecord
    └─ RenderMenu override → calls LUT picker helper, owns slot assignment + persistence

  TonemapEffect : EffectRecord
    └─ RenderMenu override → calls enum combo helper with its own label list

  ColorEffect : EffectRecord
    └─ RenderMenu override → calls color triple helper

GameMenuManager
  └─ foreach effect: effect->RenderMenu()    ← zero effect-specific knowledge

ImGui helper layer (pure functions, no game/effect domain knowledge)
  └─ ColorTriplePicker(rgb) → rgb
  └─ LUTFilePicker(files, index) → index
  └─ KeyBindPicker(dik) → dik
  └─ EnumCombo(labels, index) → index
  └─ TooltipIfHovered(text)
  └─ RevertButton(current, snapshot) → bool
```

### What This Eliminates

| Current code | Replaced by |
|---|---|
| `strcmp(node.Key, "TonemappingMode")` inline check | `TonemapEffect::RenderMenu()` override |
| `strcmp(node.Key, "KeyEnable")` inline check | Effect's own `RenderMenu()` override |
| RGB triple detection via key-name heuristic | Color effects explicitly call the color triple helper |
| LUT section hardcoded in `RenderContent()` | `LUTEffect::RenderMenu()` override |
| Tooltip rendered in 3 branches (one unreachable) | Single `TooltipIfHovered()` helper |
| Revert logic duplicated across `RenderSetting` + `RenderColorTriple` | Single `RevertButton()` helper |
| Global `s_plusPressed` / `s_minusPressed` state | Scoped inside individual widget helper calls |

### Shader Annotation Format

As part of R3, a uniform annotation format must be designed and locked before any shader editing begins. Annotations embed all metadata the engine needs to construct settings and select the correct widget — no sidecar files, no separate schema. Everything travels with the shader.

```hlsl
uniform float3 TintColor
<
    string name = "Tint Color";
    string description = "Multiplied against the final output";
    string widget = "color";
    float3 default = {1.0, 1.0, 1.0};
> = {1.0, 1.0, 1.0};

uniform int TonemapMode
<
    string name = "Tonemapping Mode";
    string description = "Tonemapping operator applied post-exposure";
    string widget = "enum";
    string enumNames = "None,Lottes,ACES,Reinhard";
    int default = 0;
>;
```

Supported widget hints cover the full range needed for shader uniforms:

| Hint | Widget rendered |
|---|---|
| *(none / default)* | Slider (float) or checkbox (bool) based on type |
| `color` | RGB color triple picker |
| `enum` | Dropdown combo (requires `enumNames`) |
| `key` | Key binding picker (DIK reference popup) |

Unknown hints fall back gracefully to the type-default widget.

**The clean boundary:**
- Shader annotation → declares data and widget hint only
- ImGui helper layer → knows how to render each widget hint
- `EffectRecord::RenderMenu()` → bridges them; no custom logic in the bridge

### Built-in Shader Annotation Pass

All existing shaders must have their uniform declarations annotated to match the finalized format. This is a large but low-risk editing pass — annotations add metadata only, no shader behavior changes. It must happen **after** the annotation format is locked, and is best delivered as a focused standalone PR.

---

## Refactor 4 — Custom Effects Drop-in Folder

Once R3 is complete and built-in shaders are annotated, the menu is fully decoupled from knowing what effects exist at compile time. The custom effects system falls out almost for free.

### Structure

```
Data/Shaders/Effects/
  NVR/              ← built-in effects (C++ EffectRecord subclasses)
  Custom/           ← drop-in folder; scanned at startup
    MyBloom/
      MyBloom.hlsl  ← only file needed; all metadata lives in annotations
    MyDOF/
      MyDOF.hlsl
```

### How It Works

- Engine scans `Custom/` at startup
- For each shader found, parses uniform annotations to build a settings list and pipeline position
- Instantiates a `GenericEffectRecord` (uses base `RenderMenu()`)
- Base `RenderMenu()` iterates the settings list and calls the appropriate ImGui helper per widget hint
- No C++ required from the shader author — ever

### Constraints

Custom effects are intentionally limited to the standard widget helper set. Any UI requirement that cannot be expressed through an annotation hint is simply not available to custom shaders. Built-in effects handle complex cases via their C++ `RenderMenu()` overrides. This is a clean and acceptable boundary.

---

## Dependency Order

```
BF-1  (bool* cast fix)                ──────────────────────────▶  ship now
BF-2  (water texture cache)           ──────────────────────────▶  ship now

R1  Type system                       ──────────────────────────▶  prerequisite for R3
R2  Texture consolidation             ──────────────────────────▶  parallel to R1

R3  EffectRecord owns UI
  ├─ 3a  Design + lock annotation format          (must come first within R3)
  ├─ 3b  Implement ImGui helper layer
  ├─ 3c  Implement base RenderMenu() + GameMenuManager wiring
  ├─ 3d  Port built-in effects to RenderMenu() overrides
  └─ 3e  Annotate all existing shaders            (after 3a is locked; own PR)

R4  Custom effects drop-in folder     ──────────────────────────▶  after R3 complete
```
