# Shader Tasks — To-Do

## Background

Working list of cheap realism/perf improvements identified during an idea-farming session on the post-process shader pipeline (`src/hlsl/NewVegas/Effects/`). Nothing in this doc has been implemented — these are researched findings and proposals, not a shipped plan. Task IDs are referenced loosely; renumber if this doc gets formalized.

Two effects here (Normal Curvature, NMS) both fake fine surface detail without more geometry, but they operate at different frequency bands and different pipeline stages — see **Composability Notes** at the bottom before implementing more than one at a time.

---

## Ambient Occlusion — Bug Fixes, Speed, Light Bounce

**File:** `src/hlsl/NewVegas/Effects/AmbientOcclusion.fx.hlsl` (Alchemy AO — hemisphere kernel + bilateral blur)

### Bugs (free wins, no tradeoff)

- **AO-1 — Dead `Samples` setting.** TOML literally says so (`resource/NewVegasReloaded.dll.defaults.toml:131,142`: `Samples = 5 # Not used (currently hardcoded)`). Plumbed C++ → shader as `AOsamples` (`AmbientOcclusion.fx.hlsl:19`) but the kernel loop uses the hardcoded `#define kernelSize 5` (line 5); the line that would read `AOsamples` is commented out (line 80). **Fix:** make the loop bound a real uniform via `[loop]` (dynamic flow control, supported on ps_3_0) so the setting actually does something, and low-end presets can default lower.
- **AO-2 — Dead `normal2` fetch.** `NormalBlurRChannel()` line 190 computes `normal2 = GetNormal(...)` per tap and never uses it. 12 wasted fetches × 2 blur passes = 24 pointless samples/pixel. Delete the line.
- **AO-3 — Duplicate depth fetch.** Same function, lines 173 and 179 both sample `TESR_DepthBuffer` at the same UV (once via `.y` directly, once via `readDepth()`). Fold into one fetch, reuse both channels.

### Speed

- **AO-4 — Half-res AO (biggest lever).** Scaffolding already exists and is dead: `#define halfres 0` (line 4), the quadrant `clip()` (lines 74–77), and a full `Expand()` upsample pass wired into the technique (lines 140–144, 218–224) — just never turned on or exposed. The two `SSAO()` passes are the most expensive part of this effect; halving resolution drops their cost to ~1/4 for a hit that's usually invisible after the existing bilateral blur (AO is low-frequency by nature). **Action:** flip it on, expose as a TOML/ImGui toggle under `Shaders.AmbientOcclusion.*`.
- **AO-5 — Merge the two `SSAO()` passes.** Currently `SSAO(io.xy)` / `SSAO(io.yx)` are one 10-sample kernel deliberately split across two full-screen draws + RT switches. Under DXVK, per-draw-call/state-switch overhead is real (see DXVK notes in `CLAUDE.md`) — merging into a single unrolled 10-tap pass trades instruction count for one fewer full-screen pass.

### Light bounce (SSGI/SSDO)

- **AO-6 — Color-bounce extension.** Reuse the existing hemisphere kernel: at each sample point, also sample `TESR_RenderedBuffer` and accumulate it (weighted by the same `dot(normal, sampleDir)` term already computed) as a second, additive channel. Add it back in `Combine()` next to the existing `ao` multiply. This is the natural, cheapest path to fake GI — same kernel, same blur infra, same settings pattern as existing `AOsamples`/`AOstrength`.
- **AO-7 — Cheap ambient/skylight tint.** Reuse `AvgLuma.fx.hlsl`'s downsample trick to get an average scene color; tint `AOclamp`'s floor toward it instead of flat gray/black. Near-zero extra cost, complements AO-6.
- **AO-8 — Temporal dither rotation.** `TESR_GameTime` is already registered and used elsewhere (`GodRays.fx.hlsl`, `Snow.fx.hlsl`). Rotating the blue-noise UV offset by it each frame breaks up the static dither pattern in the AO/GI noise without needing a new history buffer.

### Related, different file

- **AO-9 — Specular occlusion.** `Specular.fx.hlsl` never samples `TESR_NormalsBuffer` or the AO result — specular highlights render at full strength inside creases AO has already darkened. Multiply the specular contribution by the existing AO buffer (already computed earlier in the frame — see pipeline order in `ShaderManager.cpp:738-742`). Free-ish: no new sampling, just wiring an existing texture into an existing shader.

---

## Screen-Space Normal Curvature / Edge Shading (new effect)

**Proposed location:** folded into `AmbientOcclusion.fx.hlsl`'s `Combine()` pass, or a small standalone pass — reuses a buffer already bound there.

**Concept:** derive concave/convex shading purely from spatial derivatives of `TESR_NormalsBuffer` — sample a few neighboring texels, measure how much the normal diverges from its neighbors, darken concave divergence / lighten convex divergence. No ray marching, no new render target.

**Important caveat found during research:** `TESR_NormalsBuffer` (`Normals.fx.hlsl`) is **not** a material-level normal map — `ComputeNormals()` derives it purely from the depth buffer (bgolus depth-derivative technique), and `BlurNormals()` then runs a 24-tap bilateral blur specifically designed to smooth over small variation (`dropTreshold = 0.82` discards divergent samples). So this buffer has zero material bump detail (mortar lines, cobblestone dips) by the time a post-process pass could read it — only coarse geometric silhouette edges (a doorframe corner, a rock's profile) survive. Set expectations accordingly: this reads as "geometric edge shading," not a cavity/material-detail map.

**Properties:**
- Cost: near-zero (few extra texel fetches of an already-bound buffer).
- Not light-direction dependent — static darken/lighten regardless of sun angle. This is the key distinction from NMS below.
- Cannot literally double-compute the same feature NMS shadows, since it structurally can't see material bump detail at all — the two effects sit on different frequency bands and don't redundantly process the same geometry.

**Risk:** see Composability Notes — stacks with AO/specular-occlusion/NMS in the same visual regions (crevices, corners) and needs its own strength/clamp floor, not a naive multiply.

---

## Normal Mapping Shadows (NMS) — Boris Vorontsov / ENBSeries-style

**Proposed location:** per-object material shaders (`PBR.hlsl` and whichever `SLS*/PAR*` family shaders route through it) — **not** the post-process `Effects/` pipeline. By the time normals reach `TESR_NormalsBuffer` the material-level detail NMS needs is already gone (see caveat above), so this cannot be built as a post-process pass.

**Concept:** light-direction-dependent self-shadowing computed from the actual per-material tangent-space normal map at full texel resolution (brick mortar, cobblestone, cloth weave). Compares the bump's implied slope against the real light direction and attenuates lighting where the bump would self-shadow — a cheap stand-in for real parallax-occlusion self-shadowing, no ray marching. Shadows visibly shift/lengthen as the sun moves, unlike the static curvature effect above.

**Scope/risk:** the biggest, riskiest item in this doc — touches lighting correctness across most in-game object shaders (`PBR.hlsl` plus the numbered material shaders that use it), not a single self-contained file like the other two. Needs its own design pass before implementation (where exactly the light vector and tangent-space normal are already in scope per shader family, how to expose strength as a setting, etc.).

**Sequencing recommendation:** implement **after** the AO fixes and Normal Curvature pass are in and tuned. NMS will lower the baseline lit brightness of bumpy surfaces before AO/curvature/specular-occlusion (AO-9) ever run on them — tune those effects' clamp floors against NMS's output, not the other way around, or the two rounds of tuning will fight each other.

---

## Other candidates (lower priority, noted for completeness)

- **Luminance-adaptive night desaturation (Purkinje shift).** `ImageAdjust.fx.hlsl` currently does a static shadow/highlight curve tint only. Reusing the already-computed `AvgLuma.fx.hlsl` buffer to blue-shift/desaturate as scene luminance drops is one extra lerp for a real day/night realism win.
- **Sky-tinted ambient specular.** Fresnel-weighted `GetSkyColor()` (already used in `WetWorld.fx.hlsl`) applied to general PBR surfaces, not just puddles. Lives in `PBR.hlsl`, same track as NMS rather than the post-process pipeline — bigger lift, not "cheapest tier."

---

## Composability Notes

AO (AO-6 bounce term), AO-9 (specular occlusion), Normal Curvature, and NMS are all, in different ways, "darken small-scale detail" effects. They don't conflict technically — different pipeline stages, different source buffers, no shared render targets or shader-slot collisions — but they **do** overlap visually: crevices, corners, and bump detail tend to coincide, so an uncoordinated stack of independent multiplies in the same spots compounds toward a crushed/muddy image (the classic over-AO'd look from stacking too many occlusion effects without coordination).

Mitigation: give each effect its own strength/clamp floor following the precedent already in the codebase (`AOclamp` in `AmbientOcclusion.fx.hlsl:150`, `ClampStrength` in the TOML) so no combination of effects can multiply a surface all the way to black. Re-tune each effect's clamp after any new one lands, in implementation order: AO fixes/bounce → Normal Curvature → specular occlusion → NMS last (since NMS changes the baseline everything else tunes against).
