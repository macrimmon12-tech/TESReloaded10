# TESReloaded10 — NVR Preset Manager Design

## Background

NVR currently has one active settings state — whatever's in the live TOML — with no
concept of swapping visual configuration by location. Cartographer/Atlas (third-party
companion mods many NVR users already run) solve this for NVR by tagging cells and
worldspaces with keywords and layering presets on top of a default. This doc specs an
equivalent system built directly into NVR, informed by:

- an initial standalone prototype (`NVRManager` + `NVRMenu`) sent for evaluation —
  its data model and application mechanism don't fit this codebase and are not being
  ported (see "Rejected approach" below), but its shape informed the resolution order
- Cartographer's own user documentation (precedence order, default-preset-as-schema
  rule, keyword aliasing)
- a real Cartographer keyword file (`HonestHearts.ini`) confirming the on-disk format
  we're adopting directly

## Scope

**NVR settings only** (`SettingsMainStruct` / shader TOML, applied via
`SetSettingF`/`SetSettingS`). Explicitly **not** in scope: image space swapping,
lighting template swapping, or dynamic ambient lighting — Cartographer's second
domain. That territory dovetails into the separate planned **WeatherMode
restoration** work, which needs its own tweaker panel anyway; this system should be
generic enough that WeatherMode restoration can reuse the same tier/resolution
shape later, but building that domain now is out of scope.

Also explicitly not in scope, and not needed:
- A general engine-level cell/worldspace enumerator (`DataHandler`, `TES` buffers).
  The keyword files themselves already enumerate every location anyone's bothered to
  tag — see "Location picker" below.
- Any runtime dependency on JIP LN NVSE or the KEYWORDS plugin. We read the same
  on-disk file format KEYWORDS uses, parsed by our own code, with no plugin-interop
  at runtime.

## Rejected approach

The initial prototype (`NVRManager`) was evaluated in detail and rejected as a
porting target:
- Its apply path (`sprintf` a console command string, run it via
  `g_ConsoleInterface->RunScriptLine2`) has no counterpart in this codebase; we
  already have the right primitive (`TheSettingManager->SetSettingF()` /
  `SetSettingS()`, called directly).
- Its data model is a single flat `EditorID → preset name` map — one tier, no
  default/keyword/override precedence, and structurally incapable of targeting an
  individual exterior cell (the lookup key collapses to the worldspace's EditorID
  whenever `cell->worldSpace` is non-null).
- Its settings storage is a hand-rolled INI parser reinventing a subset of
  `toml11`, which is already a project dependency.
- Its UI is hand-rolled hit-testing (`AddRectFilled` + `IsMouseHoveringRect` pairs)
  duplicated three times, where plain ImGui widgets do the same job in less code.

None of this is a matter of missing headers or small fixes — the precedence model
Cartographer's users actually rely on doesn't exist in it at all. Building from
scratch against the design below is less work than adapting it.

## Precedence model

Four tiers, resolved in order, each checked only if nothing more specific matched:

```
1. TOML defaults               (resource/NewVegasReloaded.dll.defaults.toml — unconditional floor)
2. Default preset               (DefaultInterior or DefaultExterior, by cell->IsInterior())
3. Keyword preset               (the cell/worldspace's assigned keyword → its preset)
4. Cell-specific override       (always wins, even over a keyword-resolved preset)
```

Orthogonal to all four: a **Performance** layer (ported concept from the rejected
prototype's `performanceSettings`), applied last, unconditionally, regardless of
which of the four tiers resolved. A "Low" performance tier can clamp a setting even
if the active default preset didn't happen to enumerate it — **Performance is
exempt from the schema rule below**, since it's cross-cutting hardware adaptation,
not location authoring.

This is a deliberate simplification from Cartographer's own model, which allows
arbitrary N-keyword stacking with alias expansion (one tag expands into a list of
other tags, each independently resolved and stacked). We're dropping that in favor
of one keyword per cell resolving to one preset — Cartographer's stacking exists
largely to work around text-file-only authoring; NVR's live ImGui overlay means an
author can just live-tweak the final look and save it as one flattened preset
instead of composing it from fragments. See "In-game editor workflow" below for why
this still covers the same practical need (bulk-first, refine-individually-later).

### Default preset = schema, not just a fallback value

Carried over directly from Cartographer: **only Section:Key pairs present in the
active default preset (`DefaultInterior`/`DefaultExterior`) are ever eligible to be
touched by the keyword or cell-override tiers.** If `DefaultExterior` doesn't define
`Shaders.Bloom.Interiors.Blending`, no keyword or cell-specific preset can modify it
either, even if their file lists it. This keeps each preset's blast radius fully
knowable by reading one file, and prevents an obscure per-cell override from
reaching into a setting nobody expected it to touch. (Performance is the one
exception — see above.)

## Application mechanism

Modeled directly on the existing `RevertToSnapshot()` pattern
(`ImGuiManager.cpp:660`), which already does almost exactly this for the "revert
on overlay close" case:

1. Resolve the four-tier chain (+ Performance) into one merged
   `section → key → value` map, starting from TOML defaults.
2. Diff the merged map against `currentSettings` (whatever's actually live).
3. For each changed key, call `TheSettingManager->SetSettingF()` or `SetSettingS()`
   directly — no console command round-trip, this is native code in the same DLL.
4. `LoadSettings()` to refresh the cached struct.
5. Re-sync shader `Enabled` flags the same way `RevertToSnapshot` already does
   (`FillMenuSections("Shaders")` + `GetMenuShaderEnabled` + `GetEffectByName`/
   `GetShaderCollectionByName`), since a preset can toggle `Shaders.*.Status.Enabled`.

Because step 1 always starts from TOML defaults rather than layering onto whatever's
currently live, switching between any two locations (or back to a location with
nothing assigned) is correct with no separate "revert" step — same property the
rejected prototype's `UpdatePreset()` got right.

## Keyword files — adopting the existing format directly

Decision: **no new format.** We read the same on-disk convention Cartographer/
KEYWORDS already use — confirmed against a real file (`HonestHearts.ini`):

```ini
[0]
; HonestHeartsHouse
NVDLC02ZionLodge=HonestHeartsHouse,TextureConfig,Qual,Whine ; Fishing Lodge
NVDLC02ZionStation=HonestHeartsHouse,TextureConfig,Qual,Whine ; Ranger Station

; HonestHeartsCave
NVDLC02ZionSCaveW=HonestHeartsCave,TextureConfig,Qual,Whine ; Morning Glory Cave
```

- Every `.ini` in the designated folder is loaded and merged, filename-agnostic —
  per-worldspace file naming (as in this example) is purely an authoring
  convention, not something the parser depends on.
- Line format: `EditorID=keyword1,keyword2,... ; optional trailing comment`. `;`
  is used both as a full-line comment marker and as a trailing inline comment on
  data lines — the parser needs to strip trailing `; ...` before splitting on `=`,
  which the rejected prototype's parser never did.
- Blank-line/comment-block grouping (`; HonestHeartsHouse`) is purely
  organizational for the file's author; the authoritative keyword set for each
  cell is whatever's actually listed after `=`.
- **Multiple keywords per cell, resolved to our single-keyword-per-tier model**:
  for each keyword in a cell's comma list, check for a matching preset file; the
  first one found wins. In practice, most of the extra tags in existing
  Cartographer data (`TextureConfig`, `Qual`, `Whine` in the example above) won't
  correspond to any file in our own presets folder unless we create one by that
  name, so they naturally no-op rather than needing special handling.
- **Open unknown**: the leading `[0]` section header's exact meaning isn't
  confirmed — treat as a required-but-otherwise-inert wrapper unless
  implementation reveals otherwise.

## Preset files

`toml11`-based (already a project dependency), not the rejected prototype's
hand-rolled INI parser. Section/key naming matches the live TOML directly —
`[Shaders.ImageAdjust.Main]` / `Brightness = 1.141`, no leading `_` (matching
Cartographer's own convention, which strips it the same way our `SettingManager`
does internally). A preset file contains only the keys it overrides — diff-style,
not a full settings dump.

## Location resolution (`OnLocation` equivalent)

Needs a cell-change hook (not yet identified in this codebase — existing call
sites that observe `TES::currentCell` transitions should be checked before adding
a new per-frame poll). On a location change:

1. Look up a cell-specific override for the current cell's own EditorID (checked
   regardless of whether the cell has a worldspace — this is the fix for the
   rejected prototype's bug where exterior cells could never be targeted
   individually).
2. Else, look up a keyword-resolved preset — using the cell's own EditorID if
   interior, the worldspace's EditorID if exterior.
3. Else, fall through to `DefaultInterior`/`DefaultExterior` by `cell->IsInterior()`.
4. Apply via the mechanism above.

## Location picker (Cell/Worldspace browse UI)

No engine-level enumeration needed. The union of every left-hand-side EditorID
across all loaded keyword files already constitutes the full list of
preset-relevant locations — better UX than a full engine master list, since it
only surfaces places that actually have (or could have) preset-relevant tagging
rather than every untagged cell in the game.

## In-game editor workflow

1. **Bulk pass**: tag every interior in a worldspace with one shared keyword
   (via the keyword files), live-tweak settings once, save to that keyword's
   preset. Every tagged cell updates at once.
2. **Refine individually**: for any cell that needs something different, live-tweak
   and save as a cell-specific override — tier 4 wins over the shared keyword
   preset for that cell only, without touching the shared file.

This requires the editor to expose things the rejected prototype's single generic
"Export" button didn't:

- **Two distinct save actions** — "save to this cell's keyword preset" vs. "save
  as a bespoke override for this cell" — writing to different files, not one
  ambiguous export.
- **Keyword assignment inline**: "save to keyword" needs a keyword already
  assigned to the cell; if none exists, the flow should let the author assign one
  as part of saving rather than just being disabled with no explanation.
- **Blast-radius visibility**: saving to a shared keyword preset changes every
  other cell still resolving through that tier. Surface "this affects N cells" at
  save time.
- **Live tier indicator**: persistent (not just a debug-log line) UI showing
  whether the current cell is resolving via Default / Keyword / Cell-override —
  otherwise editing a keyword-governed value while standing in a cell that
  already has its own override silently does nothing visible, which is confusing
  without an explanation.

## Open items to resolve during implementation

- Exact meaning of the `[0]` keyword-file section header.
- Cell-change hook call site (does one already exist to observe `TES::currentCell`
  transitions, or does this need a new one).
- Folder/file layout for our presets and keyword directories (Cartographer's
  `Config/Cartographer/{NVR.ini, NVRPresets/, ...}` is a reasonable starting
  reference, not a requirement).
- Whether to carry over Cartographer's debug-mode (console log of current/target
  preset) and hot-reload-key ideas — both cheap, both reuse the existing `NVR*`
  console command surface.
