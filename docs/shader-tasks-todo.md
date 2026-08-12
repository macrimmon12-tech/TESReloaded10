# Shader Tasks — To-Do

Tracking doc for a batch of shader work. Each item below is a placeholder —
details, design notes, and implementation plans will be filled in by
dedicated sessions per task. Check items off as they land.

## Tasks

- [ ] **Rain shader: player motion reactivity** — add reactivity to player
  movement (e.g. rain deflection/ripple response tied to player velocity).

  <details>
  <summary>Design plan</summary>

  **Current behavior.** `src/effects/Rain.cpp`/`Rain.h` register
  `TESR_RainData`/`TESR_RainAspect` from settings only — no player state is
  read. `src/hlsl/NewVegas/Effects/Precipitations.fx.hlsl` raymarches a
  cylindrical noise volume around the camera: `toWorld()` (in
  `Effects/Includes/Depth.hlsl`) builds a *direction-only* world-space ray
  from the view/projection matrices (it never adds `TESR_CameraPosition`,
  even though that constant is declared), and `cylindrical()` maps that ray
  to `(u, v)` = (compass angle, elevation ratio). Falling is faked purely by
  scrolling `v` over time: `noiseSurfacePosition.y += SPEED * timetick`.
  There is no horizontal/wind term today, and no player position, facing,
  or velocity is wired into the shader at all.

  **Data sources (verified in-repo).**
  - `Player->pos` (`NiPoint3`, `TESObjectREFR`, `Game.h:3149`) — global
    `extern PlayerCharacter* Player` (`NewVegas/Managers.h:21`), already
    dereferenced by other effects (e.g. `MotionBlur.cpp`) with no extra
    includes needed.
  - `ShaderConst.GameTime.w` / `TESR_GameTime.w` — per-frame elapsed time,
    already registered; needed to turn a position delta into a velocity.
  - `GameState.isCellChanged` (`ShaderManager.cpp`) — guards against a
    spurious velocity spike from fast travel/door loads/teleports.
  - `TheCameraManager->IsFirstPerson()` — for optionally tuning/disabling
    the effect in third person.
  - No plain velocity field is exposed on `Actor`/`MobileObject`/
    `ActorMover` in `Game.h` — derive it via frame-to-frame delta, the same
    pattern `MotionBlurEffect` already uses for `Player->rot` (tracked
    `oldAngleZ`/`oldAngleX` members, diffed each `UpdateConstants()`).

  **CPU-side plan.** Add to `RainEffect::Constants` (`Rain.h`): a
  `D3DXVECTOR4 Wind` (xy = smoothed world-space horizontal velocity, z =
  magnitude, w = user intensity multiplier from settings) plus a stored
  `oldPos`/`havePrevPos`. In `UpdateConstants()` (`Rain.cpp`), diff
  `Player->pos` against `oldPos`, divide by `TESR_GameTime.w`, zero the
  z-component (vertical motion shouldn't tilt rain), exponentially smooth
  to kill per-frame jitter (same idea as `ShadowsExteriors`'
  `SmoothingFactor`), skip the delta entirely on `isCellChanged`, and
  register the new constant as `TESR_RainWind`. Add a `MovementInfluence`
  setting under `[_Shaders.Precipitations.Main]` in
  `resource/NewVegasReloaded.dll.defaults.toml` (auto-populates the ImGui
  slider per `SettingManager` convention) to drive the `w` multiplier.

  **GPU-side plan.** Uniformly scrolling `u` would look like the whole
  rain cylinder spinning, not leaning, since `u` has no "forward" notion —
  only compass direction. Instead, tilt the fall axis used by
  `cylindrical()` away from world-Z toward the direction opposite player
  velocity (small-angle lean, physically the same idea as wind-blown rain
  or rain on a moving car's windshield):

  ```hlsl
  float4 TESR_RainWind; // xy: smoothed world-space horizontal velocity, z: magnitude, w: MovementInfluence

  float2 cylindrical(float3 world)
  {
      float leanAmount = saturate(TESR_RainWind.z * 0.02f) * TESR_RainWind.w;
      float3 fallAxis = normalize(float3(-TESR_RainWind.xy * leanAmount, 1.0f));

      float3 tangent = world - fallAxis * dot(world, fallAxis);
      float u = -atan2(tangent.y, tangent.x) / PI;
      float v = dot(world, fallAxis) / length(tangent.xy);
      return float2(compress(u), hscale * v);
  }
  ```

  The existing `SPEED * timetick` vertical scroll is unchanged — only what
  "vertical" means per-pixel is reoriented, so drops rake toward the
  camera when moving forward, away when moving backward, and diagonally
  when strafing.

  **Caveats.** Clamp velocity magnitude (e.g. to sprint speed) so sprinting
  doesn't produce an absurd tilt; ignore vertical velocity (jumping/
  falling); skip the delta on `isCellChanged`; consider reducing/disabling
  in third person since the illusion reads best in first person. Scope is
  NV's `Precipitations.fx.hlsl` — Oblivion's parallel `Rain.fx.hlsl` is an
  older, simpler shader and would need its own pass for parity.

  </details>
- [ ] **AO shader: performance optimizations** — profile and optimize the
  existing ambient occlusion pass.
  - Bug fixes found in `src/hlsl/NewVegas/Effects/AmbientOcclusion.fx.hlsl`
    (Alchemy AO — hemisphere kernel + bilateral blur), free wins with no
    quality tradeoff:
    - **Dead `Samples` setting.** TOML says so directly
      (`resource/NewVegasReloaded.dll.defaults.toml:131,142`:
      `Samples = 5 # Not used (currently hardcoded)`). Plumbed C++ → shader
      as `AOsamples` (`AmbientOcclusion.fx.hlsl:19`) but the kernel loop
      uses the hardcoded `#define kernelSize 5` (line 5); the line that
      would read `AOsamples` is commented out (line 80). Fix: make the loop
      bound a real uniform via `[loop]` (dynamic flow control, supported on
      ps_3_0) so the setting actually does something, and low-end presets
      can default it lower.
    - **Dead `normal2` fetch.** `NormalBlurRChannel()` line 190 computes
      `normal2 = GetNormal(...)` per tap and never uses it — 12 wasted
      fetches × 2 blur passes = 24 pointless samples/pixel. Delete the line.
    - **Duplicate depth fetch.** Same function, lines 173 and 179 both
      sample `TESR_DepthBuffer` at the same UV (once via `.y` directly,
      once via `readDepth()`). Fold into one fetch, reuse both channels.
  - Speed levers:
    - **Half-res AO (biggest lever).** Scaffolding already exists and is
      dead: `#define halfres 0` (line 4), the quadrant `clip()` (lines
      74–77), and a full `Expand()` upsample pass wired into the technique
      (lines 140–144, 218–224) — never turned on or exposed. The two
      `SSAO()` passes are the most expensive part of this effect; halving
      resolution drops their cost to ~1/4 for a hit that's usually
      invisible after the existing bilateral blur (AO is low-frequency by
      nature). Action: flip it on, expose as a TOML/ImGui toggle under
      `Shaders.AmbientOcclusion.*`.
    - **Merge the two `SSAO()` passes.** `SSAO(io.xy)` / `SSAO(io.yx)` are
      one 10-sample kernel deliberately split across two full-screen draws
      + RT switches. Under DXVK, per-draw-call/state-switch overhead is
      real (see DXVK notes in `CLAUDE.md`) — merging into a single
      unrolled 10-tap pass trades instruction count for one fewer
      full-screen pass.
  - Related, different file: **specular occlusion.** `Specular.fx.hlsl`
    never samples `TESR_NormalsBuffer` or the AO result — specular
    highlights render at full strength inside creases AO has already
    darkened. Multiply the specular contribution by the existing AO buffer
    (already computed earlier in the frame — see pipeline order in
    `ShaderManager.cpp:738-742`). Free-ish: no new sampling, just wiring an
    existing texture into an existing shader.
- [ ] **AO shader: light bounce / fake GI** — extend AO to approximate
  indirect light bounce for a cheap global illumination effect.
  - **Color-bounce extension (SSDO-style).** Reuse the existing hemisphere
    kernel in `AmbientOcclusion.fx.hlsl`: at each sample point, also sample
    `TESR_RenderedBuffer` and accumulate it (weighted by the same
    `dot(normal, sampleDir)` term already computed) as a second, additive
    channel. Add it back in `Combine()` next to the existing `ao` multiply.
    This is the cheapest path to fake GI — same kernel, same blur infra,
    same settings pattern as existing `AOsamples`/`AOstrength`.
  - **Cheap ambient/skylight tint.** Reuse `AvgLuma.fx.hlsl`'s downsample
    trick to get an average scene color; tint `AOclamp`'s floor toward it
    instead of flat gray/black. Near-zero extra cost, complements the
    color-bounce extension above.
  - **Temporal dither rotation.** `TESR_GameTime` is already registered and
    used elsewhere (`GodRays.fx.hlsl`, `Snow.fx.hlsl`). Rotating the
    blue-noise UV offset by it each frame breaks up the static dither
    pattern in the AO/GI noise without needing a new history buffer.
  - Constraint: DX9 `ps_3_0` only — no compute shaders, no history/velocity
    buffer today, so this stays screen-space-only (no off-screen bounce,
    no temporal accumulation) unless a history buffer is added separately.
- [ ] **Normals: Boris' NMS technique** — implement Boris' normal map
  sampling (NMS) technique for improved normals.
  - **Concept.** Light-direction-dependent self-shadowing computed from the
    actual per-material tangent-space normal map at full texel resolution
    (brick mortar, cobblestone, cloth weave). Compares the bump's implied
    slope against the real light direction and attenuates lighting where
    the bump would self-shadow — a cheap stand-in for real
    parallax-occlusion self-shadowing, no ray marching. Shadows visibly
    shift/lengthen as the sun moves.
  - **Where it has to live.** Per-object material shaders (`PBR.hlsl` and
    whichever `SLS*/PAR*` family shaders route through it) — **not** the
    post-process `Effects/` pipeline. `TESR_NormalsBuffer` is derived
    purely from the depth buffer (`Normals.fx.hlsl`'s `ComputeNormals()`,
    the bgolus depth-derivative technique) and then heavily blurred
    (`BlurNormals()`, 24-tap, `dropTreshold = 0.82`), so by the time
    normals reach that buffer the material-level bump detail NMS needs is
    already gone. Cannot be built as a post-process pass.
  - **Scope/risk.** The biggest, riskiest item in this batch — touches
    lighting correctness across most in-game object shaders, not a single
    self-contained file. Needs its own design pass before implementation
    (where exactly the light vector and tangent-space normal are already
    in scope per shader family, how to expose strength as a setting, etc).
  - **Sequencing recommendation.** Implement after the AO fixes and the
    curvature/cavity task below are in and tuned — NMS will lower the
    baseline lit brightness of bumpy surfaces before AO/curvature/specular
    occlusion ever run on them, so those effects' clamp floors should be
    tuned against NMS's output, not the other way around.
- [ ] **Skin shader** — new shader for skin rendering (subsurface-style
  response, etc.).
- [ ] **Grass shader (maybe)** — exploratory; may not be pursued.
- [ ] **Curvature/cavity shading from normals buffer** — derive
  curvature/cavity term from the normals buffer for edge/crevice shading.
  - **Concept.** Derive concave/convex shading purely from spatial
    derivatives of `TESR_NormalsBuffer` — sample a few neighboring texels,
    measure how much the normal diverges from its neighbors, darken
    concave divergence / lighten convex divergence. No ray marching, no
    new render target; can likely fold directly into
    `AmbientOcclusion.fx.hlsl`'s `Combine()` pass since that buffer is
    already bound there.
  - **Important caveat.** As noted under the NMS task, `TESR_NormalsBuffer`
    is depth-derived and then heavily blurred — it carries no material bump
    detail (mortar lines, cobblestone dips), only coarse geometric
    silhouette edges (a doorframe corner, a rock's profile). This will read
    as "geometric edge shading," not a cavity/material-detail map — set
    expectations accordingly. It's not a substitute for NMS above; the two
    sit on different frequency bands (silhouette-scale vs.
    material-bump-scale) and don't redundantly process the same geometry,
    which is exactly why both are worth doing.
  - **Properties.** Near-zero cost; not light-direction dependent (static
    darken/lighten regardless of sun angle) — this is the key difference
    from NMS, which is dynamic and shifts with the sun.
- [ ] **Animating volumetric fog** — investigate animation of volumetric
  fog; consider whether this warrants a separate lowfog/mist shader.

## Notes

Add design notes, references, and implementation details under each task
as they're worked on.

**Composability / stacking risk (relevant to the two AO tasks, curvature,
and NMS above):** all four are, in different ways, "darken small-scale
detail" effects. They don't conflict technically — different pipeline
stages, different source buffers, no shared render targets or shader-slot
collisions — but they *do* overlap visually: crevices, corners, and bump
detail tend to coincide, so an uncoordinated stack of independent
multiplies in the same spots compounds toward a crushed/muddy image (the
classic over-AO'd look from stacking too many occlusion effects without
coordination). Mitigation: give each effect its own strength/clamp floor
following the precedent already in the codebase (`AOclamp` in
`AmbientOcclusion.fx.hlsl:150`, `ClampStrength` in the TOML) so no
combination can multiply a surface all the way to black. Suggested
implementation order, tuning each against the previous: AO fixes/bounce →
curvature/cavity → specular occlusion → NMS last (since NMS changes the
baseline everything else tunes against).

**Other cheap ideas surfaced during AO/GI research, not yet tracked as
their own tasks:**
- Luminance-adaptive night desaturation (Purkinje shift) — `ImageAdjust.fx.hlsl`
  currently does a static shadow/highlight curve tint only; reusing the
  already-computed `AvgLuma.fx.hlsl` buffer to blue-shift/desaturate as
  scene luminance drops is one extra lerp for a real day/night realism win.
- Sky-tinted ambient specular — Fresnel-weighted `GetSkyColor()` (already
  used in `WetWorld.fx.hlsl`) applied to general PBR surfaces, not just
  puddles. Same track as NMS (lives in `PBR.hlsl`), not the post-process
  pipeline — bigger lift, not "cheapest tier."
