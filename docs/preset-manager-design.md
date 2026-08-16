# TESReloaded10 — NVR Preset Manager Design

## Background

NVR currently has one active settings state — whatever's in the live TOML — with no
concept of swapping visual configuration by location. Cartographer/Atlas (third-party
companion mods many NVR users already run) solve this for NVR by tagging cells and
worldspaces and layering presets on top of a default. This doc specs an equivalent
system built directly into NVR.

This is v2 of the design, revised after further discussion diverged from
Cartographer's own model in a few deliberate ways — see "Deliberate departures from
Cartographer" below for what changed and why.

### Rejected approach

An initial standalone prototype (`NVRManager` + `NVRMenu`) was evaluated and
rejected as a porting target — not adapted, replaced:
- Its apply path (`sprintf` a console command string, run it via
  `g_ConsoleInterface->RunScriptLine2`) has no counterpart in this codebase; the
  right primitive is `TheSettingManager->SetSettingF()`/`SetSettingS()`, called
  directly.
- Its data model is a single flat `EditorID → preset name` map — one tier, no
  precedence, and structurally incapable of targeting an individual exterior cell
  (the lookup key collapses to the worldspace's EditorID whenever `cell->worldSpace`
  is non-null).
- Its settings storage is a hand-rolled INI parser reinventing a subset of
  `toml11`, already a project dependency.
- Its UI is hand-rolled hit-testing (`AddRectFilled` + `IsMouseHoveringRect` pairs)
  duplicated three times, where plain ImGui widgets do the same job in less code.

## Scope

**NVR settings only** (`SettingsMainStruct` / shader TOML, applied via
`SetSettingF`/`SetSettingS`). Explicitly **not** in scope: image space swapping,
lighting template swapping, or dynamic ambient lighting — Cartographer's second
domain. That territory dovetails into the separate planned **WeatherMode
restoration** work, which needs its own tweaker panel anyway.

Also not needed: any engine-level cell/worldspace enumerator, or any runtime
dependency on JIP LN NVSE / the KEYWORDS plugin. Everything location-related is
driven entirely by our own keyword files, read and cached at boot.

Also explicitly out of scope for the preset manager itself: a Cell/Worldspace
browse-and-`coc`/`cow` picker (present in the original prototype's Cell/Worldspace
tabs). Nothing in this design needs it — assignment is always done by physically
standing in a location. It's a genuinely useful utility on its own merits, so it's
being added to the existing **NVR Dev Tools** panel (`ImGuiManager.cpp:472`)
instead, independent of and unrelated to presets.

## Deliberate departures from Cartographer

- **No runtime merging between Default / Keyword / Override.** Cartographer
  composites an arbitrary stack of diff-style presets on every location change.
  We don't — see "Data model" below.
- **One keyword per cell, not a stackable list with alias expansion.** Cartographer
  lets a cell carry many keyword tags simultaneously, each independently resolved
  and layered, with an alias table that expands one tag into several. We use a
  single keyword per cell, full stop.
- **No dependency on the external KEYWORDS plugin's file format.** We read our own
  format (see below), not Cartographer's own `EditorID=keyword1,keyword2,...`
  convention.
- **Exteriors don't use a keyword tier at all** — see "Resolution order."

## Data model: presets are complete, standalone snapshots

**Default**, **Keyword**, and **Override** presets each contain a full copy of
every setting they intend to control — not a diff, not something merged with a
lower tier at runtime. Exactly one of these wins per location. There is no
"default preset defines the allowed key set" constraint — any preset may define
any key it wants, independent of what any other preset defines.

Practical consequence: editing the Default preset does **not** propagate into any
Keyword or Override preset — they're independent files, not diffs layered on
Default. The only thing that *can* ripple a change across every preset at once is
a **Variant** (see below), which is the sole diff-style, stacking mechanism in the
whole system.

## Resolution order

**Interior** (checked in order, first match wins):
1. Cell-specific **Override**, if this exact cell has one
2. **Keyword** preset, if the cell has a keyword tag *and* a preset file named
   identically to that keyword exists
3. **Default** (`DefaultInterior`)

**Exterior** (checked in order, first match wins):
1. Worldspace-specific **Override**, if this worldspace has one
2. **Default** (`DefaultExterior`)

No keyword tier for exteriors. Reasoning: there are relatively few worldspaces,
and each one either wants a genuinely distinct full look (→ gets an Override) or
is fine inheriting Default. This has a useful side effect: a worldspace added by
some *other* mod, with no NVR-side authoring for it at all, falls straight through
to `DefaultExterior` automatically rather than matching nothing.

Then, for both interior and exterior alike: any active **Variants** are applied as
diffs on top of whichever base preset resolved.

## Keyword files

Sections are keywords; cells are bare entries listed underneath:

```ini
[HonestHeartsCave]
NVDLC02ZionSCaveW
NVDLC02PrefabCaves

[HonestHeartsHouse]
NVDLC02ZionLodge
NVDLC02ZionStation
```

- Read and cached once at boot.
- **Interior cells only** — worldspaces never appear here, since exteriors don't
  use the keyword tier.
- Reassigning a cell's keyword is an **offline** operation — cut its line, paste
  it under a different section, in a text editor. No in-game control retags a
  cell's keyword; the in-game editor only assigns Override/Default/Keyword
  *presets*, never keyword *membership*.
- A keyword's preset file is named identically to the keyword (`HonestHeartsCave`
  → `HonestHeartsCave.ini`), so "does this keyword have a preset" is a plain
  file-existence check.

## Preset files

`toml11`-based, matching the live TOML's section/key naming
(`[Shaders.ImageAdjust.Main]` / `Brightness = 1.141`, no leading `_`). Each file is
a full standalone snapshot, not a diff. Four kinds of preset, stored separately:

| Kind | Count | Filename |
|---|---|---|
| Default | 2, fixed | `DefaultInterior.ini`, `DefaultExterior.ini` |
| Keyword | one per keyword | `<KeywordName>.ini` |
| Override (interior) | one per tagged cell | `<CellEditorID>.ini` |
| Override (exterior) | one per tagged worldspace | `<WorldspaceEditorID>.ini` |
| Variant | up to 5 | `<VariantName>.ini` |

**Never cached — always read fresh from disk at the point of use** (a location
resolving, the Reload button, a Load-browser selection), unlike keyword files.
Presets are written in-session via the Save actions below, so caching their
content would risk the cache going stale relative to what was just saved — this
way there's nothing to keep in sync, the file on disk is simply the truth. The
"Preset browser / Load" list only needs *filenames* to populate itself and tag
each row's kind — classified by name lookup against the two reserved Default
names and the cached keyword set (see "Folder layout" below) — actual content
is read on demand only once something is selected and loaded.

## Variants

Up to 5, independently toggleable via checkbox, **end-user facing** — a preset
pack author can ship several alongside their location presets and document which
ones a player should enable (e.g. a Performance variant for weaker hardware, a
color-grade variant for taste). Any combination can be active simultaneously.
Applied on top of whichever base preset resolved, for both interior and exterior
locations.

**Authoring flow:**
1. Load any base preset — doesn't matter which.
2. Make only the intended change(s) in the live editor (e.g. reduce
   `ShadowCascade` and nothing else).
3. Click **Save Variant** — captures exactly the key(s) changed since step 1
   began, as absolute values (e.g. `ShadowCascade = <new value>`), nothing else.
4. That diff is safe to replay on top of any other base preset later, since it's
   a fixed absolute override rather than something relative to the preset it was
   authored against.

**Persisting which Variants are enabled**: which toggles are currently on is
global-to-install state that needs to survive a restart, but it's the *only*
thing in this whole system that isn't already derivable from "does a file
exist" (see "Do we need a master file?" below). Rather than route it through
`SettingManager`/the main NVR TOML — which would require adding a new entry to
the shipped defaults TOML just to satisfy `FillNode`'s typed-accessor
requirement, and would inherit the main settings system's explicit-Save-button
persistence model — this gets its own small, dedicated file, independent of the
main settings GUI entirely:

```ini
[EnabledVariants]
Performance
WarmTone
```

Read once at boot alongside the keyword files. Written immediately whenever a
Variant checkbox is toggled in-game — not deferred to any "Save Settings"
action, since this file has nothing to do with the main settings persistence
lifecycle.

### Do we need a master file?

Considered and rejected, beyond the small file above. Everything else in this
design is already self-describing from the filesystem: a cell's keyword
membership lives in the keyword files; a preset existing at
`<KeywordName>.ini` / `<CellEditorID>.ini` *is* the record that it's assigned;
the catalog of what presets/variants exist is just a directory listing. A
general-purpose master index tracking any of that separately would be a second
source of truth that can drift from the actual files (delete a preset
externally, now a master file has a stale reference to reconcile). Enabled-
Variant state is the one genuine exception, since it's a user preference layered
on top of the catalog rather than a fact recoverable from any file's existence
— hence the small dedicated file above, scoped to exactly that and nothing more.

## Folder layout

Proposed, not load-bearing for anything else in this design — easy to adjust.
Default/Keyword/Override presets are stored **flat**, distinguished purely by
filename lookup, not by subfolder:

```
Data\NVR\
  Keywords\                  membership files (section = keyword, cells listed under)
    HonestHearts.ini
    Freeside.ini
  Presets\
    DefaultInterior.ini      reserved name
    DefaultExterior.ini      reserved name
    HonestHeartsCave.ini     Keyword preset
    Casino.ini               Keyword preset
    NVDLC02ZionLodge.ini     Override, interior cell
    NVDLC02TheStrip.ini      Override, exterior worldspace
  Variants\
    Performance.ini
    WarmTone.ini
  EnabledVariants.ini        not a preset — persisted UI state, kept out of Presets\
```

Kind-per-subfolder was dropped as unnecessary: it never actually prevented a
collision or saved any classification work. FNV EditorIDs are already globally
unique across form types, so a flat `<EditorID>.ini` naming scheme was always
collision-free between interior and exterior Overrides regardless of folder.
And the Keyword-vs-Override distinction is something the resolution logic
already has to compute by cross-referencing the cached keyword set and the
current location's EditorID — folder location wasn't providing any information
that lookup doesn't already need to produce anyway. Consistent with the same
"don't let two things claim to know the same fact" principle behind not caching
presets and not building a master index file.

The one rule this introduces: `DefaultInterior` and `DefaultExterior` are
reserved filenames — a keyword can't be named either, or classification breaks.

`Keywords\` and `Variants\` stay physically separate from `Presets\`, since both
are genuinely different in kind rather than just differently-named: Keywords are
an entirely different file format (membership lists, not settings snapshots),
and Variants are diffs rather than full snapshots, browsed through a completely
separate panel of checkboxes rather than the tiered preset browser.

## Debug/authoring tooling — in-game log window

Cartographer's debug-mode idea (console log of current preset/tier on every
switch) is better served here by an actual **in-game log window** rather than a
toggle that dumps to the console. `Logger::Log()` (`src/base/Logger.cpp:145`)
currently only writes straight to `LogFile` via `vfprintf_s` — no in-memory
buffering. Mirroring each formatted message into a small capped ring buffer
(`std::deque<std::string>`, same pattern as ImGui's own `ExampleAppLog` in
`imgui_demo.cpp`) alongside the existing file write gets a live, scrollable
in-game log for free — and since it hooks `Logger::Log` itself rather than
tailing the log file back off disk, it captures everything logged anywhere in
the codebase, not just preset-manager activity. Two ways to narrow it down,
both included: a free-text `ImGuiTextFilter` box for general use, plus a
**"Preset only" checkbox** for the common case. The checkbox requires every
preset-manager log call to carry a consistent tag (e.g. a `[Preset]` prefix) to
filter against — settling the earlier open question: yes, a tagging convention
is needed, not just optional. Fits naturally in the same **NVR Dev Tools** panel
as the relocated Cell/Worldspace picker above, as a general utility rather than
something preset-manager-specific.

Cartographer's global hot-reload-key idea (re-scan every keyword/preset file
without restarting) is **not** being carried over — keyword reassignment is
deliberately offline-only and already requires a full game restart to take
effect, so there's no reason for preset changes to be hot-reloadable while
keyword changes aren't. Presets already read fresh from disk on every use (see
"Preset files" below) — "Reload current preset" (see "In-game UI — location
assignment" below) covers the in-session case that actually matters, at no
extra implementation cost.

## Application mechanism

Triggered once per actual cell transition — gated on
`TheShaderManager->GameState.isCellChanged` (`ShaderManager.cpp:245`), an
existing edge-triggered flag already set every frame in
`ShaderManager::UpdateConstants()` by comparing `Player->parentCell` against a
persisted `PreviousCell` member. No new cell-change hook needed.

1. Resolve which single base preset wins (per "Resolution order" above) and
   **read its file fresh off disk** — presets are never cached (see "Preset
   files" above), so this is a real file open/parse every time, not a cache
   lookup. Its full contents become the target map directly; no merging with
   any other tier.
2. Layer each currently-enabled Variant's diff on top of the target map —
   also read fresh from disk, same rule.
3. Diff the target map against `currentSettings` (what's actually live).
4. For each changed key, call `TheSettingManager->SetSettingF()` or `SetSettingS()`
   directly — no console command round-trip, this is native code in the same DLL.
5. `LoadSettings()` to refresh the cached struct.
6. Re-sync shader `Enabled` flags the same way `RevertToSnapshot` already does
   (`FillMenuSections("Shaders")` + `GetMenuShaderEnabled` + `GetEffectByName`/
   `GetShaderCollectionByName`).

Because step 1 always resolves fresh from disk rather than layering onto
whatever's currently live, moving between any two locations is correct with no
separate "revert" step — and it's also exactly what makes "Reload current
preset" trivial: that button is just this same sequence, re-run on demand.

## In-game UI — location assignment

Always operates on wherever the player currently is — assignment is done by
physically visiting a location and using contextual buttons, not by browsing to
and targeting a remote location.

**Interior** — three status indicators, distinguishing *shown* (this tier is
potentially relevant) from *highlighted* (this tier is the one currently active):

| Indicator | Shown when | Highlighted when |
|---|---|---|
| Default | always | nothing more specific applies |
| Keyword | cell has a keyword tag, even with no preset authored yet | a preset file matching that keyword exists and is active |
| Override | a preset already exists for this exact cell | it's the active tier |

Three matching save buttons, saving the full current live state (all keys the
editor exposes, not a diff), each with escalating warnings:
- **Save to Default** — warns it rewrites the floor for *every* interior in the game
- **Save to Keyword** — warns with a live count pulled from the keyword file
  ("this affects N cells")
- **Save to Override** — warns only if one already exists here to overwrite

**Exterior** — two status indicators and two save buttons, same shown/highlighted
pattern, covering only Default and Override (no Keyword row, per "Resolution
order" above).

**Unsaved/previewing state**: while a preset is loaded via the browser below but
not yet saved (see next section), the status indicators switch to a distinct
**Unsaved** state instead of their normal shown/highlighted display — the
location's actually-resolved tier hasn't changed, only the live editing state has,
and this needs to stay visibly obvious. Walking to a different location without
saving silently discards the preview and returns to normal resolution; no
additional warning beyond the Load confirmation itself.

**Reload current preset** — a separate button, always available, distinct from
the Load browser below: discards any live unsaved edits (including an
in-progress preview from Load, if one is active) and resets to whatever's
actually assigned to this location. Nothing bespoke under the hood — since
presets are always read fresh from disk anyway (see "Preset files" above),
this is just the normal "Application mechanism" resolve step, re-run on
demand. Same "you'll lose unsaved changes" risk as Load, so it gets the same
OK/Cancel treatment.

## In-game UI — Preset browser / Load

A persistent, always-visible list of every existing **Default**, **Keyword**, and
**Override** preset — Variants excluded, since they're diffs rather than
standalone starting points. Each row is tagged with its kind; **Keyword presets
are grouped first**, ahead of Default and Override.

- Select a preset, click **Load**.
- OK/Cancel confirmation: *"This will override your current unsaved settings."*
- Confirmed load replaces the live editing state wholesale with that preset's
  full contents. Nothing is written to disk — the source preset file is
  untouched, and the current location's actual resolved assignment is unchanged
  until an explicit Save.

Typical workflow: arrive at a new, untagged interior (currently showing Default)
→ browse the list for an existing preset that looks close to the desired result
→ Load it as a working starting point → tweak live → **Save to Keyword** or
**Save to Override** under the *current* location's own identity, leaving the
borrowed preset untouched.

## In-game UI — Variants

Separate from location assignment — a panel of up to 5 checkboxes reflecting
which Variants are currently enabled (global toggles, not scoped per-location),
plus the **Save Variant** authoring flow described above.

## Open items to resolve during implementation

None currently outstanding — the last one (global hot-reload wiring) was
resolved by dropping that feature entirely; see "Debug/authoring tooling"
above.
