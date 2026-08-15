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
    string defaultValue = "";
>;
```

When the user picks a file, the base `RenderMenu()` calls `LUTEffect::OnPathChanged("DayLUT", filename)`, which calls `GetFileTexture()`, rebinds the slot, and updates `DayCellCount` from the texture dimensions. The `OnPathChanged` hook is the only LUT-specific code needed — all widget rendering is generic.

### Shader Annotation Format (Locked)

Annotations are written as HLSL `< >` annotation blocks attached to each uniform declaration. Everything the engine needs to construct settings and render the correct widget lives in the shader — no sidecar files, no separate schema.

> **Correction (post-R3d, verified against a real build):** the field was originally named `default`, but the D3DX9 effect compiler rejects it with `error X3000: syntax error: unexpected token 'default'` — it's a reserved HLSL keyword and cannot be used as an annotation field name, even though it parses fine as a struct/table key everywhere else this style of format is documented. Renamed to `defaultValue` throughout this section and in `EffectRecord::GetUniformAnnotation()`. If you're annotating a shader by hand from an older copy of this doc, use `defaultValue`, not `default`.

#### Required vs Optional Fields

Only `defaultValue` is required. Everything else is optional with defined fallbacks.

| Field | Type | Required | Fallback if omitted |
|---|---|---|---|
| `defaultValue` | matches uniform type | **Yes** | — |
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
| `hidden` | None — uniform is skipped when the parser builds the settings list | any |
| `packed` | None on the uniform itself — explodes into N independent settings rows, one per `componentKeys` entry (see Packed Components below) | `float2`/`float3`/`float4` |

`hidden` is for uniforms that are engine-supplied (camera/projection transforms, sun/weather state, per-frame timers, light lists, etc.) or otherwise not user-configurable — the shader still declares them normally and the `<>` block still documents what the uniform carries, but the annotation parser excludes them from the menu-building pass entirely rather than rendering a widget nobody should touch. This is distinct from simply omitting `widget`: an omitted `widget` still gets the type-default slider/checkbox/drag-int treatment (and would show up in a future annotation-driven settings list), whereas `hidden` opts a documented uniform out of that list on purpose.

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

#### Packed Components

**Why this exists:** almost every real NVR shader constant is a `float4` packing 2–4 *independent* settings into one uniform (e.g. `TESR_BloomData.x/.y/.z/.w` = filter radius x, filter radius y, `Shaders.Bloom.Main.PassBlending`, and a derived `1/Passes` — three different TOML keys and one derived value, in one register). The one-name/description/default/widget-per-uniform model above only cleanly describes a uniform that *is* one setting (a lone scalar like `TESR_BloomFinalGain`) or *is* one widget in its own right (an RGB triple with `widget = "color"`). `packed` is for everything else — the common case, not the exception.

A `packed` uniform's `<>` block carries parallel comma-delimited fields — same delimiter convention as `enumNames`. **These six fields are positionally aligned with each other only, never with the uniform's own x/y/z/w GPU layout.** `componentKeys` is simply an ordered bag of the SettingManager keys this uniform's annotation chooses to expose; a component with no real user-facing setting (an engine-computed slot, e.g. `TESR_BloomData.x`/`.y`, the filter-radius pair) is never mentioned at all rather than represented by a placeholder gap — there is no requirement that the list's length or order say anything about which literal component backs which key. (This matters because nothing downstream reads a packed uniform's live GPU value through this index — see "Consumption" below — so there's nothing for positional x/y/z/w alignment to serve.)

```hlsl
float4 TESR_BloomData
<
    string widget = "packed";
    // Only 2 of this uniform's 4 components back a real setting -- .x/.y are
    // engine-computed filter radii (see Bloom.fx.hlsl) and are simply never
    // mentioned here, not represented by an empty placeholder entry.
    string componentKeys     = "PassBlending,Passes"; // SettingManager keys this uniform's annotation exposes
    string componentNames    = "Pass Blending,Passes"; // optional; falls back to the componentKeys entry, underscores -> spaces
    string componentDefaults = "0.0,8";
    string componentMins     = "0,2";
    string componentMaxs     = "1,8";
    string componentSteps    = "0.01,1";
    float defaultValue = 0.0; // still the required field per the table above; unused for packed uniforms
>;
```

Only `componentKeys` is meaningful on its own (it's what turns a component into a settings-list row); `componentNames`/`componentDefaults`/`componentMins`/`componentMaxs`/`componentSteps` are each optional per the same fallback rules as their singular counterparts, applied per-entry by position within these lists. A malformed or missing `componentKeys` degrades the same way an empty `enumNames` does — no rows for that uniform, no error, no crash.

**Consumption (R3f-2):** `EffectRecord::BuildPackedSettingsIndex()` walks every `packed`-widget uniform on an effect's own constant table once (whenever the effect loads) and builds `key -> {Name, Min, Max, Step}` for every `componentKeys` entry across the whole effect. `RenderMenuNode()` consults this index only when the direct `"TESR_" + effect name + key` lookup finds nothing (or finds something explicitly `hidden`) — a persisted setting's *value* still flows through `SettingManager` exactly as it always has; only where the widget's label/tooltip/range comes from changes. One consequence of not tracking positional GPU layout: there is no int-vs-float type-aware fallback for a missing `componentMin`/`componentMax`/`componentStep` entry the way there is for a scalar uniform (`GetUniformAnnotation()` can inspect a lone uniform's own declared type; it can't infer what a `packed` uniform's *n*-th exposed key is logically supposed to be, since the uniform itself is always float-typed at the GPU level regardless of the setting's own type) — missing entries always fall back to the float defaults (`0`/`1`/`0.001`). Authors backing an int-typed setting through `packed` should supply explicit `componentMins`/`componentMaxs`/`componentSteps` rather than relying on the fallback.

`packed` is orthogonal to (and never combined with) `color`/`enum`/`key`/`path`/`hidden` — a uniform is either one thing the widget hints already describe, or a bag of `componentKeys` describes, never both.

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
    float defaultValue = 1.0;
    float min = 0.0;
    float max = 4.0;
    float step = 0.01;
>;

uniform float3 TintColor
<
    string name = "Tint Color";
    string description = "Multiplied against the final output";
    string widget = "color";
    float3 defaultValue = {1.0, 1.0, 1.0};
> = {1.0, 1.0, 1.0};

uniform string DayLUT
<
    string name = "Day LUT";
    string widget = "path";
    string folder = "Data/Textures/NewVegasReloaded/LUTs/";
    string filter = "*.dds,*.png";
    string defaultValue = "";
>;
```

**The clean boundary:**
- Shader annotation → declares data, defaults, range, and widget hint only
- ImGui helper layer → knows how to render each widget hint
- `EffectRecord::RenderMenu()` → bridges them; no custom logic in the bridge
- `OnPathChanged()` → optional per-effect hook for side-effects only

### Built-in Shader Annotation Pass ✓ DONE (R3e)

All 35 NVR `EffectRecord` shaders under `src/hlsl/NewVegas/Effects/*.fx.hlsl` have their top-level uniform declarations annotated (`name`/`description`/`defaultValue`/`widget` where appropriate, `min`/`max`/`step` for sliders needing a non-default range) plus a `PipelinePosition` declaration at the top of each file, matching the render order in `ShaderManager::RenderEffectsPreTonemapping()`/`RenderEffects()`. `ShaderCollection` shaders (Grass, PBR, POM, Skin, Sky, Terrain, Tonemapping, Water) were excluded as planned — no constant table to annotate against. `LUTEffect` additionally gained `DayLUT`/`NightLUT`/`InteriorLUT` string uniforms (`widget = "path"`, `folder = "Data/Textures/NewVegasReloaded/LUTs/"`) declaring the target shape for its still-pending R3f port off the hand-rolled cycle-picker special case. The 7 shared engine-global uniforms declared once in `Includes/Depth.hlsl` (camera/projection/view transforms) were annotated at that single shared declaration site rather than duplicated per including shader. Metadata only — no shader behavior changes.

---

## Refactor 4 — Custom Effects Drop-in Folder

Once R3 is complete and built-in shaders are annotated, the menu is fully decoupled from knowing what effects exist at compile time.

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
- Instantiates a `GenericEffectRecord` (uses base `RenderMenu()`)
- Custom effect settings persist to an INI section named after the shader file
- No C++ required from the shader author — ever
- Custom effects cannot be `ShaderCollection` types; they are always `EffectRecord` subclasses

---

## Dependency Order

```
BF-1  ✓ DONE
BF-2  ✓ DONE

R1a  Typed settings storage           ✓ DONE
R2   Texture consolidation            ✓ DONE

R3   EffectRecord owns UI
  ├─ 3a  Annotation format              ✓ LOCKED (defaultValue, not default — see R3a Correction note; X3000 on real compiler)
  ├─ 3b  ImGui helper layer             ✓ DONE
  ├─ 3c  Base RenderMenu() + wiring     ✓ DONE (delegate lives in ImGuiManager.cpp, not GameMenuManager — see note below)
  └─ 3d  HLSL annotation parser         ✓ DONE
       ↓ first testable state: one EffectRecord (Bloom or Coloring)
         rendered end-to-end via annotations                        ✓ DONE — Bloom's TESR_BloomFinalGain,
                                                                        verified against a real build + live playtest
  ├─ 3e  Annotate all NVR EffectRecord shaders   ✓ DONE — all 35 shaders,
       PipelinePosition + LUT path-widget uniforms; `hidden` widget added
       and applied to every engine-supplied/other-effect-owned uniform
  └─ 3f  Port all effects to annotation-driven RenderMenu()
          ├─ 3f-1  `packed` widget + componentKeys/Names/Defaults/Mins/Maxs/Steps
          │        schema + parser plumbing                              ✓ DONE
          ├─ 3f-2  Per-effect PackedSettings index (EffectRecord::
          │        BuildPackedSettingsIndex(), built in LoadEffect() after
          │        CreateCT()) wired into RenderMenuNode as the fallback
          │        when the "TESR_" + EffectName + Key direct convention
          │        finds nothing (or finds something `hidden`) -- first
          │        visibly testable payoff (real labels/tooltips/ranges
          │        sourced from shader annotations for e.g. Bloom's
          │        PassBlending/Passes, same persisted values, same TOML
          │        section/key model)                                    ✓ DONE (Bloom only so far)
          │        ↓ remaining: retrofit componentKeys onto every other
          │          effect's own packed uniforms (the ones R3e marked
          │          NOT hidden -- TESR_CinemaData, TESR_GodRaysRay,
          │          TESR_ColoringData, etc.), batched like R3e            ← NEXT
          ├─ 3f-3  (stretch) retire remaining hardcoded menu heuristics
          │        (RGB-triple-by-key-suffix, LUT hardcoded section) now that
          │        a real per-effect annotation index exists to drive them
          ├─ ShaderCollection overrides (8 effects) — hand-written, legitimate
          └─ LUTEffect — annotation-driven via path widget + OnPathChanged() hook

  Why `packed` instead of unpacking every effect's constants into one scalar
  uniform per setting: almost every real NVR setting today is packed 2-4 to a
  float4 (TESR_BloomData, TESR_CinemaData, TESR_GodRaysRay, ...) -- unpacking
  would mean rewriting shader math and GPU register layout across all 35
  shaders' HLSL and C++ RegisterConstants()/UpdateConstants(), a large
  behavior-risking change for a UI metadata refactor. `packed` keeps 3f at
  the same "declares data, defaults, range, widget hint only" character as
  every other step in R3 -- no shader math changes, ever.

  Note on 3c: ImGuiManager.cpp, not GameMenuManager.cpp, is the actual live
  NV settings UI (see CLAUDE.md's DXVK-input notes) — GameMenuManager::Render()
  is dead code for NV, only still called from the Oblivion render hook.
  EffectRecord::RenderMenu()/RenderGenericSection() are called from
  ImGuiManager::RenderContent() instead; GameMenuManager.cpp was not touched.

  Known follow-up filed during 3b+c+d's live playtest, not yet done: a
  generic "(?)" popup exposing the DIK scancode reference table somewhere
  in the overlay chrome (keybinds are staying as plain int scancodes, not
  becoming a `key`-widget picker — see docs discussion in that session).

R1b  TOML → INI                       ────────────────────────▶  after R3 structure is final

R4   Custom effects drop-in folder    ────────────────────────▶  after R3 + R1b complete
```
