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
- [ ] **AO shader: light bounce / fake GI** — extend AO to approximate
  indirect light bounce for a cheap global illumination effect.
- [ ] **Normals: Boris' NMS technique** — implement Boris' normal map
  sampling (NMS) technique for improved normals.
- [ ] **Skin shader** — new shader for skin rendering (subsurface-style
  response, etc.).
- [ ] **Grass shader (maybe)** — exploratory; may not be pursued.
- [ ] **Curvature/cavity shading from normals buffer** — derive
  curvature/cavity term from the normals buffer for edge/crevice shading.
- [ ] **Animating volumetric fog** — investigate animation of volumetric
  fog; consider whether this warrants a separate lowfog/mist shader.

## Notes

Add design notes, references, and implementation details under each task
as they're worked on.
