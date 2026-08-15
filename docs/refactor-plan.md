# TESReloaded10 — ImGui / Effect Architecture Refactor Plan

## Background

This plan consolidates feedback from code review and developer discussion identifying structural problems in the ImGui settings overlay, the settings type system, and the effect/shader pipeline. The goal is to eliminate spaghetti rendering code, fix active bugs, and establish an architecture that supports a drop-in custom effects system without requiring engine changes for each new effect.

---

## Session Continuity

Some refactor steps (particularly R3b+c+d) are large enough to risk hitting context limits mid-session. Follow these rules for every implementation session:

**Commit incrementally, not at the end.** Commit after each sub-step completes even if nothing is testable yet. If the session dies, the next session can read the branch and continue from the last commit rather than starting over.

**Write a handoff note if context runs low.** If a session is running low on context before finishing, it must:
1. Commit whatever is done to the branch
2. Write `docs/r3-handoff.md` (or equivalent) describing: what is complete, what is in progress, what decisions were made, and exactly what the next session needs to do to continue
3. Stop — do not push broken or half-finished code without the handoff note

**Continuing a stalled session.** Start the next session with:
> "Read `docs/refactor-plan.md` and `docs/r3-handoff.md`. Continue from where the previous session left off. Do not redo completed work."

**Clean up handoff notes after merging.** Once a step is complete and merged, delete the handoff note — it is scaffolding, not permanent documentation.

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
- Existing user settings will reset on upgrade — no migration tool needed (settings are graphics tweaks, not critical data)
- `EffectRecord` subclasses write and read their own INI section by effect name
- Custom effects in `Custom/` get their own INI section automatically by shader name

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

The menu currently holds all effect-specific knowledge. `RenderContent()`, `RenderSetting()`, and `RenderColorTriple()` mix display logic, input handling, tooltip rendering, settings persistence, special-case widget branches, and hardcoded field name checks in a single call chain. Every new effect with non-standard UI requires modifying `ImGuiManager`.

### Target Architecture

```
EffectRecord (base class)
  virtual RenderMenu(ImGuiContext)
    └─ default impl: iterate settings, call appropriate ImGui helper per widget hint
  virtual OnPathChanged(uniformName, newPath)   ← optional hook for effects that need
    └─ default: no-op                               side-effects on path selection

GameMenuManager
  └─ foreach effect: effect->RenderMenu()    ← zero effect-specific knowledge

ImGui helper layer (pure functions, no game/effect domain knowledge)
  └─ ColorTriplePicker(rgb) → rgb
  └─ FilePicker(folder, filter, current) → filename
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
| LUT section hardcoded in `RenderContent()` | `path` widget hint on DayLUT/NightLUT/InteriorLUT uniforms |
| Tooltip rendered in 3 branches (one unreachable) | Single `TooltipIfHovered()` helper |
| Revert logic duplicated across `RenderSetting` + `RenderColorTriple` | Single `RevertButton()` helper |
| Global `s_plusPressed` / `s_minusPressed` state | Scoped inside individual widget helper calls |

### EffectRecord vs ShaderCollection — Two Rendering Tiers

NVR has two distinct effect base classes with fundamentally different architectures:

**`EffectRecord` subclasses (34 effects)** — use `ID3DXEffect*` with an accessible constant table. HLSL uniform annotations are readable at runtime via `GetAnnotation()`. These are fully annotation-driven after R3.

**`ShaderCollection` subclasses (8 effects)** — use pre-compiled `.vso`/`.pso` binary shaders with no constant table to enumerate. Settings live in plain C++ structs pushed to GPU via hardcoded constant registers. These **cannot** use the annotation system and require hand-written `RenderMenu()` overrides.

| ShaderCollection | Complexity | Notes |
|---|---|---|
| GrassShaders | Low | Single D3DXVECTOR4 |
| POMShaders | Low | Single D3DXVECTOR4 |
| SkinShaders | Low | Two D3DXVECTOR4s |
| SkyShaders | Low | Contains a runtime-computed `bool` with no INI backing |
| PBRShaders | Medium | 5 weather-context sub-structs × 5 floats |
| TerrainShaders | High | Weather sub-structs + parallax + LOD + bool flags |
| TonemappingShaders | High | 3 weather sub-structs × 16 floats + mode combo |
| WaterShaders | High | 2 full structs × 5 vectors + globals |

These overrides are legitimate uses of the virtual dispatch mechanism, not gaps in the architecture. They are isolated to the `ShaderCollection` layer and do not affect the generic annotation-driven path for `EffectRecord` effects.

### LUTEffect — Annotation-Driven via Path Widget

`LUTEffect` is an `EffectRecord` subclass and **is** fully annotation-driven. Its Day/Night/Interior texture slots are expressed as `string` uniforms with `widget = "path"`:

```hlsl
uniform string DayLUT
<
    string name = "Day LUT";
    string widget = "path";
    string folder = "Data/Textures/NewVegasReloaded/LUTs/";
    string filter = "*.dds,*.png";
    string default = "";
>;
```

When the user picks a file, the base `RenderMenu()` calls `LUTEffect::OnPathChanged("DayLUT", filename)`, which calls `GetFileTexture()`, rebinds the slot, and updates `DayCellCount` from the texture dimensions. The `OnPathChanged` hook is the only LUT-specific code needed — all widget rendering is generic.

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
| `hidden` | Not shown in menu — use for engine-supplied or non-configurable uniforms | any |

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
string folder = "Data/Textures/NewVegasReloaded/LUTs/";
string filter = "*.dds,*.png";
```

The engine scans `folder` at overlay open and presents the file list with prev/next arrows. The uniform value is the selected filename. Effects that need side-effects on selection implement `OnPathChanged()`.

#### Pipeline Position

Declared once at the shader level, outside any uniform block, at the top of the file.

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

uniform string DayLUT
<
    string name = "Day LUT";
    string widget = "path";
    string folder = "Data/Textures/NewVegasReloaded/LUTs/";
    string filter = "*.dds,*.png";
    string default = "";
>;

// Engine-supplied uniforms use widget = "hidden" to document without exposing in menu
uniform float4 TESR_SunsetColor
<
    string name = "Sunset Color";
    string description = "Sun color near horizon, supplied by engine from active weather. Not user-configurable.";
    string widget = "hidden";
>;
```

**The clean boundary:**
- Shader annotation → declares data, defaults, range, and widget hint only
- ImGui helper layer → knows how to render each widget hint
- `EffectRecord::RenderMenu()` → bridges them; no custom logic in the bridge
- `OnPathChanged()` → optional per-effect hook for side-effects only

### Built-in Shader Annotation Pass

All existing NVR `EffectRecord` shaders must have their uniform declarations annotated. `ShaderCollection` shaders are excluded — they have no constant table to annotate against. Delivered as a focused standalone PR after the annotation parser (R3d) is working.

---

## Refactor 4 — Custom Effects Drop-in Folder

Once R3 is complete and built-in shaders are annotated, the menu is fully decoupled from knowing what effects exist at compile time.

### Engine Data Availability

A key concern raised during planning was whether custom shaders could access engine data (depth buffer, camera position, sun direction, etc.) without C++ backing. Research confirmed that **engine data is largely automatic** via NVR's existing `ConstantsTable` mechanism:

- `ShaderManager::Initialize()` registers ~30 global engine values (`TESR_ViewTransform`, `TESR_CameraPosition`, `TESR_SunDirection`, `TESR_GameTime`, `TESR_FogColor`, etc.) as live C++ pointers, updated once per frame
- When any shader's `SetCT()` runs, it looks up each `TESR_`-prefixed uniform by name in `ConstantsTable` and binds it automatically
- Globally registered textures (`TESR_DepthBuffer`, `TESR_NormalsBuffer`, `TESR_AvgLumaBuffer`) bind automatically to any shader that declares them, provided the owning effect is loaded
- **No per-effect C++ required** for any of the globally registered engine data

The one real barrier is **effect registration** — a shader dropped in `Custom/` needs to be dynamically instantiated as a `GenericEffectRecord` and registered with `ShaderManager`. That is exactly what R4 implements.

**What custom shaders can access for free** (via `TESR_` uniform declaration):
- View/projection matrices, camera position, frustum data
- Sun direction, sun color, ambient color
- Game time, time of day
- Fog color and density
- Weather blend weights
- Depth buffer, normals buffer, average luma buffer
- Any other globally registered engine constant

**What requires C++ (not available to custom shaders):**
- Effect-specific derived values (e.g. `TESR_BloomData`, `TESR_DOFData`) — these are registered by individual built-in effects and not globally available
- Custom engine data not already in `ConstantsTable`
- Complex update semantics with interdependencies between settings

For porting shaders from ReShade or similar frameworks: remap ReShade uniform semantics to NVR `TESR_` equivalents. A porting guide documenting the mapping is worth producing as part of R4.

### Structure

```
Data/Shaders/Effects/
  NVR/              ← built-in effects (C++ EffectRecord subclasses)
  Custom/           ← drop-in folder; scanned at startup
    MyBloom/
      MyBloom.hlsl  ← only file needed; all metadata lives in annotations
```

### How It Works

- Engine scans `Custom/` at startup
- For each shader found, parses uniform annotations to build a settings list and pipeline position
- Instantiates a `GenericEffectRecord`, registers it with `ShaderManager` dynamically
- `GenericEffectRecord::RegisterConstants()` calls `RegisterConstant()` for any effect-specific values declared in annotations
- All globally registered `TESR_` engine data binds automatically via `SetCT()`
- Custom effect settings persist to an INI section named after the shader file
- No C++ required from the shader author for any globally available engine data

### Constraints

- Custom effects are always `EffectRecord` subclasses — the `ShaderCollection` architecture is not available to custom shaders
- Effect-specific derived constants from built-in effects are not available unless those effects are loaded
- Complex update semantics requiring interdependencies between settings require a built-in C++ effect

---

## Dependency Order

```
BF-1  ✓ DONE
BF-2  ✓ DONE

R1a  Typed settings storage           ────────────────────────▶  prerequisite for R3
R2   Texture consolidation            ────────────────────────▶  parallel to R1a

R3   EffectRecord owns UI
  ├─ 3a  Annotation format              ✓ LOCKED
  ├─ 3b  ImGui helper layer             ┌─ commit after each sub-step
  ├─ 3c  Base RenderMenu() + wiring     ├─ write handoff note if context runs low
  └─ 3d  HLSL annotation parser         └─ nothing testable until all three done
       ↓ first testable state: one EffectRecord (Bloom or Coloring)
         rendered end-to-end via annotations
  ├─ 3e  Annotate all NVR EffectRecord shaders   (own PR)
  └─ 3f  Port all effects to annotation-driven RenderMenu()
          └─ ShaderCollection overrides (8 effects) — hand-written, legitimate
          └─ LUTEffect — annotation-driven via path widget + OnPathChanged() hook

R1b  TOML → INI                       ────────────────────────▶  after R3 structure is final

R4   Custom effects drop-in folder    ────────────────────────▶  after R3 + R1b complete
  └─ produce TESR_ → ReShade semantic porting guide as part of R4
```
