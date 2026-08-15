# TESReloaded10 — ImGui / Effect Architecture Refactor Plan

## Background

This plan consolidates feedback from code review and developer discussion identifying structural problems in the ImGui settings overlay, the settings type system, and the effect/shader pipeline. The goal is to eliminate spaghetti rendering code, fix active bugs, and establish an architecture that supports a drop-in custom effects system without requiring engine changes for each new effect.

---

## Bug Fixes (Ship Immediately, Independent)

### BF-1 — Pointer Cast UB in SettingManager ✓ DONE
**File:** `src/core/SettingManager.cpp:1223` and `:1243`

`(bool*)GetSettingI(...)` casts an `int` return value to a `bool*` pointer instead of `bool`. Fixed.

### BF-2 — Uncached Water Texture in OcclusionManager ✓ DONE
**File:** `src/core/OcclusionManager.cpp:226`

Direct `D3DXCreateTextureFromFileA` call bypassing `TextureManager`. Fixed.

Note: this texture is a debug-only occlusion map visualization and is never player-visible. The fix is for caching consistency only — in-game water texture switching is not possible via this path; the visible water surface is rendered by the game engine's own shaders which NVR does not control.

---

## Refactor 1 — Type System (Settings Storage)

**Root cause documented at:** `src/core/SettingManager.cpp:56`

All settings are stored as strings internally and re-parsed on every access. This forces every consumer to hand-parse types, causes bool and int values to silently route through float code paths, and requires defensive double-casts such as `(bool)atof(Node.Value)` throughout `SettingManager.cpp` and `ImGuiManager.cpp`.

**Target state:**
- Settings carry a proper typed value (`bool`, `int`, `float`, `string`) rather than raw strings
- `SetSetting` has correctly typed overloads — no implicit bool → float promotion
- Consumers read typed values directly; no `strcmp(value, "1")` or `atof` at call sites
- The defaults TOML is eliminated entirely — defaults now live in shader annotations (see R3)
- The user save file is migrated from TOML to INI (see R1b below)

### Refactor 1b — Replace TOML with INI for User Save File

This is a significant sub-project within R1 and should be planned and executed carefully.

**Current role of TOML:**

| Role | After refactor |
|---|---|
| Default values | ✓ Eliminated — live in shader annotations |
| Type information | ✓ Eliminated — live in HLSL uniform declarations |
| Setting discovery | ✓ Eliminated — engine discovers from shader annotations |
| Tooltip descriptions | ✓ Eliminated — live in shader annotations |
| User’s saved values | **Remains — needs a new home** |

Once the schema responsibilities are gone, TOML is doing nothing a simpler format couldn’t do. The TOML parser library dependency is no longer justified.

**Target format: INI**

INI is already the native format of the game and xNVSE ecosystem, requires no library dependency, parses faster, and is trivially human-readable for debugging. The user save file becomes a flat key/value store — one section per effect, one key per uniform:

```ini
[Bloom]
Intensity=0.75
Threshold=0.85

[LUT]
DayTexture=warm_lut.dds
NightTexture=cool_lut.dds

[Tonemapping]
TonemapMode=2
Exposure=1.1
```

**What this involves:**
- Replace `SettingManager`’s TOML read/write backend with an INI reader/writer
- Remove the TOML parser library from the project
- Migrate any existing user TOML save files (one-time conversion or accept that settings reset on upgrade)
- Ensure `EffectRecord` subclasses write and read their own section by effect name
- Custom effects in `Custom/` get their own INI section automatically by shader name

**Migration note:** existing users will have their settings stored in TOML. Recommendation: silent reset on upgrade. The settings are graphics tweaks, not critical user data.

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
| `strcmp(node.Key, "KeyEnable")` inline check | `key` widget hint on the uniform annotation |
| RGB triple detection via key-name heuristic | `color` widget hint on the uniform annotation |
| LUT section hardcoded in `RenderContent()` | `path` widget hint on the uniform annotation |
| Tooltip rendered in 3 branches (one unreachable) | Single `TooltipIfHovered()` helper |
| Revert logic duplicated across `RenderSetting` + `RenderColorTriple` | Single `RevertButton()` helper |
| Global `s_plusPressed` / `s_minusPressed` state | Scoped inside individual widget helper calls |

### Known Override Exception: Tonemapping

Tonemapping is a `ShaderCollection`, not an `EffectRecord` subclass. Its mode value lives in a plain C++ struct field baked into shared HLSL via constant registers — there is no uniform declaration to annotate. It cannot be driven by the annotation parser and requires a hand-written `RenderMenu()` override with a hardcoded label array. This is a legitimate use of the virtual override escape hatch, not a gap in the architecture.

### Shader Annotation Format (Locked)

Annotations are written as HLSL `< >` annotation blocks attached to each uniform declaration. Everything the engine needs to construct settings and render the correct widget lives in the shader — no sidecar files, no separate schema.

#### Required vs Optional Fields

Only `default` is required. Everything else is optional with defined fallbacks.

| Field | Type | Required | Fallback if omitted |
|---|---|---|---|
| `default` | matches uniform type | **Yes** | — |
| `name` | `string` | No | Variable name, underscores → spaces |
| `description` | `string` | No | No tooltip rendered |
| `widget` | `string` | No | Type-default (see widget table) |

#### Widget Hints

| Hint | Widget rendered | Applies to |
|---|---|---|
| *(omitted)* | Type-default (see below) | any |
| `color` | RGB color triple picker | `float3` |
| `enum` | Dropdown combo (requires `enumNames`) | `int` |
| `key` | Key binding picker (DIK reference popup) | `int` |
| `slider` | Explicit slider | `int` (overrides drag-int default) |
| `path` | File cycle picker — prev/next arrows + filename display | `string` |

**Type-defaults when `widget` is omitted:**

| HLSL type | Default widget |
|---|---|
| `float`, `float2`, `float3`, `float4` | Slider(s) |
| `int` | Drag int |
| `bool` | Checkbox |

Unknown hints fall back silently to the type-default — no error, no crash.

#### Range and Step

Optional fields for slider uniforms. Defaults apply when omitted.

| Field | `float` default | `int` default |
|---|---|---|
| `min` | 0.0 | 0 |
| `max` | 1.0 | 10 |
| `step` | 0.001 | 1 |

`step` controls both drag speed and the +/- button increment.

#### Enum Labels

Declared as a single comma-delimited string in `enumNames`. Zero-indexed — value `0` maps to the first label.

```hlsl
string enumNames = "None,Lottes,ACES,Reinhard";
```

Spaces within labels are valid; commas are the delimiter.

#### Path Widget

Requires a `folder` field. `filter` is optional, defaults to `*.dds`.

```hlsl
string widget = "path";
string folder = "Data\\Textures\\LUT\\";
string filter = "*.dds";
```

The engine scans `folder` at overlay open and presents the file list with prev/next arrows. The uniform value is the selected filename.

#### Pipeline Position

Declared once at the shader level, outside any uniform block, at the top of the file. Controls which render stage the effect runs in.

| Value | When it runs | Use for |
|---|---|---|
| `PreTonemapping` | Before the game's HDR tonemapper | Lighting, AO, bloom, exposure, atmospheric |
| `PostTonemapping` | After tonemapper on LDR output | Color grading, lens, AA, sharpening, vignette |

Default if omitted: `PostTonemapping`.

```hlsl
string PipelinePosition = "PostTonemapping";
```

#### Full Example

```hlsl
string PipelinePosition = "PostTonemapping";

uniform float Exposure
<
    string name = "Exposure";
    string description = "Scene exposure multiplier";
    float default = 1.0;
    float min = 0.0;
    float max = 4.0;
    float step = 0.01;
>;

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

uniform string LUTTexture
<
    string name = "LUT Texture";
    string widget = "path";
    string folder = "Data\\Textures\\LUT\\";
    string filter = "*.dds";
    string default = "";
>;
```

**The clean boundary:**
- Shader annotation → declares data, defaults, range, and widget hint only
- ImGui helper layer → knows how to render each widget hint
- `EffectRecord::RenderMenu()` → bridges them; no custom logic in the bridge

### Built-in Shader Annotation Pass

All existing NVR shaders must have their uniform declarations annotated to match the finalized format. This is a large but low-risk editing pass — annotations add metadata only, no shader behavior changes. Delivered as a focused standalone PR after the annotation parser (R3d) is working.

**Goal:** the widget hint set was designed by working backwards from every special case in the built-in effects. After the annotation pass, no built-in effect should require a C++ `RenderMenu()` override except Tonemapping (documented above). The virtual override mechanism exists as an escape hatch for that and any genuinely unforeseen future cases.

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
- Custom effect settings persist to an INI section named after the shader file
- No C++ required from the shader author — ever

### Constraints

Custom effects are limited to the standard widget hint set by design. The hint set already covers every widget needed by the current built-in effects (excepting Tonemapping which is a known `ShaderCollection` exception). If a genuinely novel widget type is needed in the future, the correct fix is to add a new hint to the engine — not to require C++ from shader authors.

---

## Dependency Order

```
BF-1  (bool* cast fix)                ✓ DONE
BF-2  (water texture cache)           ✓ DONE

R1a  Typed settings storage           ────────────────────────▶  prerequisite for R3
R2   Texture consolidation            ────────────────────────▶  parallel to R1a

R3   EffectRecord owns UI
  ├─ 3a  Annotation format design        ✓ LOCKED (see spec above)
  ├─ 3b  ImGui helper layer
  ├─ 3c  Base RenderMenu() + GameMenuManager wiring
  |        + one real EffectRecord stubbed end-to-end as smoke test
  |        (use Bloom or Coloring — NOT Tonemapping, it is a ShaderCollection)
  ├─ 3d  Build the HLSL annotation parser
  ├─ 3e  Annotate all existing NVR shaders   (own PR, after 3d working)
  └─ 3f  Port all remaining built-in effects to annotation-driven RenderMenu()
          Tonemapping gets a hand-written override (documented exception)

R1b  TOML → INI save file             ────────────────────────▶  after R3 structure is final

R4   Custom effects drop-in folder    ────────────────────────▶  after R3 + R1b complete
```
