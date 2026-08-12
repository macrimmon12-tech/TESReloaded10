# Derived Metallicness — Spec/Gloss Heuristic

## Status

**Deferred.** Blocked on the in-flight PR adding a real per-material metallic
texture slot. Do not start implementation until that PR lands — see
[Relationship to the authored metallic map](#relationship-to-the-authored-metallic-map).
This doc specs the design so it's ready to pick up once that groundwork is in.

## Background

A contributor found a way to attach new texture maps to materials, which
makes a real metallic map possible — but authoring tens of thousands of
metallic maps for existing NV assets is not tractable. This spec covers a
runtime fallback: derive a "close enough" per-pixel metallic value from data
NV assets *already ship with* (diffuse + spec/gloss), so every asset in the
game gets a plausible metallic response on day one, with zero new art.

## What's already in the codebase

Traced against the current object lighting path, not assumed:

- `src/hlsl/NewVegas/Shaders/ObjectTemplate.hlsl` samples only `BaseMap`
  (diffuse) and `NormalMap`. NV's normal maps pack **specular/gloss
  intensity in the alpha channel** — `ObjectTemplate.hlsl:526`:
  `float roughness = getRoughness(normal.a);`. This is the one piece of
  spec/gloss data guaranteed to be bound on every object draw call already;
  no new sampler is needed to read it.
- `src/hlsl/NewVegas/Shaders/Includes/PBR.hlsl` already implements the
  correct metal/dielectric split: `reflectance = lerp(0.04, albedo,
  metallicness)` (metals: F0 = albedo, no diffuse term; dielectrics: F0 ≈
  4%, diffuse retained). The BRDF math is correct and unchanged by this
  work.
- `src/hlsl/NewVegas/Shaders/Includes/Object.hlsl:79-109` — `getSunLighting`,
  `getPointLightLighting`, `getPointLightLightingAtt` all call
  `PBRDiffuse(0, ...)` / `PBR(0, ...)` with **metallicness hardcoded to
  `0`**. Every object in the game currently renders as 100% dielectric.
- `src/effects/PBR.cpp` / `PBR.h` already register a global `Metallicness`
  TOML setting (`Shaders.PBR.{Default,Rain,Night,NightRain,Interiors}`)
  feeding `TESR_PBRData.x` — but nothing in `Object.hlsl` reads
  `TESR_PBRData.x`; the literal `0` above is dead-ends the setting for
  objects. This is the integration point: replace that `0` with a computed
  per-pixel `metallic` value.

## Physical grounding

Real metals are defined by *no diffuse term + high, colored F0*. NV predates
any metallic/roughness workflow, so artists had only diffuse + spec-in-
normal-alpha to fake a metal look — a flat Lambertian "metal" with a weak
highlight reads as plastic. That means wherever an artist pushed spec
intensity hard, they were very likely trying to sell a metal, polished, or
mirror-like surface. The vanilla spec/gloss channel already encodes usable
metal-ness intent; this spec just extracts it.

## The heuristic

```hlsl
specScore = smoothstep(SpecThresholdLow, SpecThresholdHigh, normal.a);   // vanilla spec/gloss alpha
chroma    = saturation(baseColor.rgb);                                   // HSV S, or (max-min)/max
metallic  = saturate(specScore * (1 - chroma * SaturationInfluence) * MetallicBias);
```

- **`specScore`** — primary signal. Gates on how hard the original artist
  pushed the specular highlight. Cloth/skin/most plastic sit low; polished
  or bare metal (weapons, ammo casings, tin cans, power armor plates) sits
  high. No extra texture fetch — `normal.a` is already in a register at
  this point in `ObjectTemplate.hlsl`.
- **`chroma`** — secondary signal, damping only. Steel/iron/brass/gunmetal
  diffuse albedo tends to run low-to-mid saturation; saturated-but-glossy
  dielectrics (red plastic, painted signage, glossy cloth) get pulled back
  down without being hard-zeroed.
- **`MetallicBias`** — global curve/fudge factor, tunable per preset the
  same way `Metallicness` already is.

Cost: one `smoothstep`, one saturation calc, one `saturate` — no new
sampler, no new render target.

### Known false positives

Wet surfaces and glass/ice also read as high-spec, but those mostly route
through separate systems already (`WetWorld.fx.hlsl`, dedicated water/glass
shaders) rather than this exact `Object.hlsl` path. Realistic edge cases to
spot-check once implemented: glossy ceramic, lacquered wood, ragdoll skin
sheen.

## Relationship to the authored metallic map

This heuristic is a *fallback*, not a competing system. Once the new
metallic-map slot lands:

```hlsl
metallic = lerp(heuristicMetallic, authoredMetallicMap.r, hasAuthoredMap);
```

Day one: 100% coverage via heuristic, everywhere. As real maps get authored
per-asset, they silently override the guess — no regression risk, no visible
seam between "has a real map" and "doesn't yet." This turns the tens-of-
thousands-of-maps problem from a blocker into an incremental backlog.

**Why this is blocked on that PR rather than built in parallel:** the
fallback's `lerp` needs whatever sampler/flag the metallic-slot work uses to
signal "a real map is bound for this material" (and possibly matches its
NIF-side loader/naming conventions). Building the fallback against a guess
at that interface risks rework once the real shape lands. Pick this back up
once the metallic-slot PR is merged.

## Implementation plan (once unblocked)

1. **Settings** — add to `resource/NewVegasReloaded.dll.defaults.toml`
   under `Shaders.PBR.*` (mirroring existing `Metallicness` per-preset
   entries so it auto-populates in the ImGui menu via `SettingManager`):
   - `SpecThresholdLow`, `SpecThresholdHigh`
   - `SaturationInfluence`
   - `MetallicBias`
2. **Shader** — add `getDerivedMetallic(float specAlpha, float3 albedo)` to
   `PBR.hlsl`; call it from `Object.hlsl` in place of the hardcoded `0`
   passed to `PBRDiffuse`/`PBR`/`PBRSpecular`/`PBRSun`/`PBRSunSpecular`.
3. **C++** — extend `PBRShaders::UpdateSettings()` /
   `PBRShaders::Constants` (`src/effects/PBR.cpp`, `PBR.h`) to read and pack
   the new thresholds/bias into a constant register, following the existing
   `Data`/`ExtraData` pattern.
4. **Fallback wiring** — once the authored map slot exists, add the
   `lerp(heuristic, authoredMap.r, hasAuthoredMap)` step described above.
5. **Debug visualization** — resurrect the commented-out debug output path
   in `ObjectTemplate.hlsl` (~lines 528-538, currently wired for roughness)
   to render the derived metallic mask in-game, for tuning against known
   reference assets (weapons, armor, ammo, tin cans vs. glossy cloth/
   ceramic/skin false positives).
