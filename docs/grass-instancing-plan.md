# Grass Rendering: True Instancing + Overdraw Reduction — Plan & Spec

## Background

Grass is a reported major performance sink, especially with mods that push
density past vanilla defaults. Investigation (see conversation history /
this doc's companion analysis) found two independent cost buckets:

1. **CPU draw-call/state submission overhead.** The engine's native grass
   shaders (`GRASS23x000`–`GRASS23x003`, reverse-engineered and ported with
   forward-shadow support on `claude/objecttemplate-lightfix-eval` —
   **read-only reference, not a dependency; relevant pieces get ported
   onto this branch as needed**) already use a "constant-array instancing"
   trick: one prototype mesh drawn per batch, up to `GRASS_INSTANCE_COUNT
   = 228` blades per batch, with each blade's position/scale/orientation
   pulled from a `float4 InstanceData[228]` vertex-shader constant array
   (`c20`–`c247`) indexed by a per-vertex instance index. The 228 cap is a
   hard SM3.0 register-file ceiling, not a design choice — dense grass
   forces many small batches per cell.
2. **GPU fill-rate/overdraw.** Per-pixel shadow sampling
   (`GetSunShadow`/`GetShadowGeometricNormal`, with `ddx`/`ddy`) and
   lighting math run once per overlapping blade layer per screen pixel.

This plan attacks both, in order: real GPU instancing removes the 228 cap
and collapses per-cell batch count (bucket 1), then three overdraw
mitigations (bucket 2) ride on top of the same pipeline, and two further
tricks (distance decimation, grazing-angle thinning) become nearly free
once we own the per-frame instance list.

**Explicitly out of scope for this plan:** LOD extension (rendering grass
beyond the engine's own cutoff distance). That requires independently
reproducing/approximating the engine's placement algorithm and is a
separate, larger effort — see prior discussion. Nothing here blocks it,
and the instance-buffer-ownership work in Phase 1 is a prerequisite for it,
but it is not attempted here.

**Sequencing:** Phase 1 (instancing) → Phase 2 (overdraw: shadow-to-vertex,
alpha-test + ordering, front-to-back sort) → Phase 3 (distance decimation,
grazing-angle thinning). Phase 2 items 2.1–2.3 do not strictly require
Phase 1, but Phase 3 does, and validating Phase 2 against a real instanced
draw path (rather than the soon-to-be-replaced constant-array path) avoids
throwaway work. Build in this order.

---

## Phase 1 — True GPU Instancing (GI-1..GI-5)

### GI-1 — Capture points in the device wrapper

**Files:** `src/core/Device/Device.cpp`, `src/core/Device/Device.h`

`TESRDirect3DDevice9` already wraps every D3D9 call as a transparent
pass-through (`Device.cpp:342-343` `DrawIndexedPrimitive`, `:382-383`
`SetVertexShader`, `:390-391` `SetVertexShaderConstantF`). No new hooking
mechanism is needed — add state and branches directly to these three
methods.

Add to `TESRDirect3DDevice9` (or a small owned `GrassInstancer` member):

```cpp
bool grassShaderBound = false;          // current VS is one of the 4 grass VS handles
IDirect3DVertexShader9* grassVS[4] = {}; // handles for GRASS23x000..003, registered at shader load
```

Registration: the four grass `ShaderRecord` objects already own their
compiled `ShaderHandle` (pattern used at `RenderPass.cpp:10`,
`SetVertexShader(VertexShader->ShaderHandle, false)`). Add a call from
`GrassShaders` (`src/effects/Grass.cpp`) at shader-load time that hands
these four handles to the device wrapper's registry — a `RegisterGrassShaders(IDirect3DVertexShader9* handles[4])`
call added to `Device.h`/`.cpp`, invoked once from `GrassShaders::RegisterConstants()`
or an equivalent init path.

**`SetVertexShader` (`Device.cpp:382`):** before forwarding, set
`grassShaderBound = (pShader is in grassVS[])`.

**`SetVertexShaderConstantF` (`Device.cpp:390`):** if `grassShaderBound`
and `StartRegister == 20`, copy `pConstantData` (up to `Vector4fCount`
float4s, ≤228) into a per-frame accumulator keyed by
`(currentMesh, currentTexture)` instead of forwarding to the real device.
`currentMesh`/`currentTexture` are read off whatever state the wrapper
already has visibility into at draw time (vertex buffer + stream 0
pointer, and the bound sampler-0 texture) — captured at flush time in
GI-2, not here; this call only needs to stash the constants under the key
that will be finalized when the matching draw call arrives.

**`DrawIndexedPrimitive` (`Device.cpp:342`):** if `grassShaderBound`,
do **not** forward to `D3DDevice->DrawIndexedPrimitive(...)`. Instead:
read the currently bound stream-0 vertex buffer and sampler-0 texture,
finalize the key for the instance data captured above, append
`(key, instanceCount, instanceData[])` to the per-frame accumulator, and
return `D3D_OK`. The engine believes the draw happened; it never reaches
the GPU in this form.

All other shaders continue through untouched — this only special-cases
the four known grass vertex shader handles.

### GI-2 — Per-frame accumulator and flush

**New files:** `src/core/GrassInstancer.cpp` / `.h`

Data structure:

```cpp
struct GrassBatchKey {
    IDirect3DVertexBuffer9* mesh;   // prototype mesh identity
    IDirect3DIndexBuffer9*  indices;
    IDirect3DTexture9*      diffuseTex;
    IDirect3DVertexShader9* variant; // which of the 4 GRASS23x0xx shaders
};

struct GrassInstance {
    D3DXVECTOR4 posScale;   // xyz = world pos, w = scale   (matches vanilla InstanceData layout)
    D3DXVECTOR4 orientFrac; // packed as vanilla does: frac(xyz)=orientation, frac(w)=lightScale
};

std::unordered_map<GrassBatchKey, std::vector<GrassInstance>, KeyHasher> frameAccumulator;
```

`GI-1`'s capture path pushes into `frameAccumulator[key]` — no dedup
needed beyond the map key; multiple 228-instance batches for the same
mesh/texture/variant across multiple cells simply grow the same vector.

**Flush trigger:** end of the grass render sub-pass. The engine renders
grass as a contiguous pass (all grass draws happen together per frame,
between terrain and other opaque/transparent passes) — detect "leaving
grass" the same way `grassShaderBound` is detected: the next
`SetVertexShader` call to a non-grass handle triggers `Flush()` before
forwarding that call. This needs no new pass boundary — it falls out of
GI-1's existing hook.

`Flush()`, for each `(key, instances)` in the accumulator:
1. Ensure a dynamic per-instance `IDirect3DVertexBuffer9` sized for
   `instances.size()` exists for this key (grow-only pool, reused
   frame-to-frame — allocate once for the largest seen instance count per
   key, `D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY`, `D3DPOOL_DEFAULT`).
2. `Lock()` with `D3DLOCK_DISCARD` (first write this frame for that
   buffer) and memcpy the instance vector in.
3. `SetStreamSource(0, key.mesh, 0, protoStride)` — the single prototype
   vertex (per-vertex position/uv/color from the original mesh).
4. `SetStreamSource(1, instanceVB, 0, sizeof(GrassInstance))`.
5. `SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | instances.size())`.
6. `SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1)`.
7. `SetIndices(key.indices)`, bind `key.diffuseTex` to sampler 0.
8. `SetVertexShader(streamedVariantFor(key.variant))` (GI-3).
9. Real `D3DDevice->DrawIndexedPrimitive(...)` with
   `primCount = protoIndexCount/3 (or /1 for line lists, matching
   original topology) ` and instance count folded into the frequency
   flags above (D3D9 instancing: primitive count stays per-instance;
   the frequency flag on stream 0 is what replicates it `instances.size()`
   times).
10. Reset both streams' frequency to 1 (`D3DSTREAMSOURCE_INDEXEDDATA | 1`)
    afterward so unrelated subsequent draws aren't affected.
11. Clear the accumulator for the next frame.

No cap on `instances.size()` beyond `IDirect3DVertexBuffer9` size limits —
this is the entire point.

### GI-3 — Streamed vertex shader variants

**New files:** `src/hlsl/NewVegas/Shaders/GRASS23x000S.vso.hlsl` (and
`001S`/`002S`/`003S`), derived from the four variants on
`claude/objecttemplate-lightfix-eval` (ported onto this branch, not edited
in place there).

Change from each vanilla-derived variant: replace the constant-array
lookup with a second-stream per-instance input, same semantics as the
constant data it replaces:

```hlsl
struct VS_INPUT {
    float4 position : POSITION;    // stream 0, unchanged
    float4 color    : COLOR0;      // stream 0
    float2 uv       : TEXCOORD0;   // stream 0
    // stream 1, one element per instance (replaces `instance : TEXCOORD1` + InstanceData[] lookup)
    float4 instPosScale   : TEXCOORD1;   // xyz = world pos, w = scale
    float4 instOrientFrac : TEXCOORD2;   // frac(xyz) = orientation, frac(w) = lightScale
};
```

Everything after `float4 inst = InstanceData[idx];` in the original stays
identical — `inst` is now `IN.instPosScale`/`IN.instOrientFrac` combined,
same math (`frac(inst)` for orientation, `inst.w` for scale, `inst.xyz`
for position). `GRASS23x001S`/`003S` additionally carry the real `NORMAL`
input on stream 0 unchanged, per their vanilla variants. Register layout
simplifies significantly since `InstanceData[228]` and its `c20`–`c247`
reservation disappear — the shadow-matrix register hazard called out in
every vanilla-derived file's header comment goes away, freeing registers.
Keep the same constant layout for `DiffuseDir`/`WindData`/etc. (`c0`–`c17`)
for consistency with the pixel shader and `ShaderRecord` constant
registration, which doesn't change.

New vertex declaration needed (`IDirect3DVertexDeclaration9`) describing
the two-stream layout with `SetStreamSourceFreq` semantics — build once at
init, reused for all four variants (same stream-1 layout across all of
them).

Pixel shaders (`GRASS23x000TMS.pso.hlsl` etc.) are unchanged in this
phase — Phase 2's GI-2.1 touches them.

### GI-4 — Settings and fallback

**Files:** `src/effects/Grass.h/.cpp`, `resource/NewVegasReloaded.dll.defaults.toml`

Add `[Shaders.Grass.Main] Instancing = true` (bool, ImGui-auto-populated
per `SettingManager` convention). When `false`, `GI-1`'s hooks pass every
call straight through unmodified — this is the existing vanilla path and
must remain fully intact as a fallback/A-B toggle for regression testing
and troubleshooting user reports.

### GI-5 — Validation

- Visual: grass renders identically (position, orientation, wind sway,
  lighting, fog) with `Instancing` on vs. off, static and moving camera,
  day/night, wind on/off, forward shadows on/off.
- Correctness: instance count parity — sum of vanilla per-batch instance
  counts for a fixed cell view should equal total instances in the flushed
  buffers for that frame (add a debug counter/overlay).
- Perf: draw-call count for grass in a fixed dense test cell, before vs.
  after (expect large reduction, see prior discussion's guesstimate:
  10–50x fewer grass draw calls in dense scenes). Frame time in the same
  scene, before vs. after.
- Edge cases: cell transitions (buffers must not carry stale data across a
  cell load/unload — accumulator is cleared every frame regardless, so
  this should be safe by construction, but verify no leak/growth in the
  dynamic-buffer pool over a long play session).

---

## Phase 2 — Overdraw Reduction (GO-1..GO-3)

Builds on the streamed shaders from GI-3.

### GO-1 — Move shadow sampling from pixel to vertex shader

**Files:** `GRASS23x000S..003S.vso.hlsl`, `GRASS23x000TMS.pso.hlsl` (and
the `002`/`003`-shared PS variant)

Currently (per-fragment, in the PS): `GetShadowGeometricNormal` (uses
`ddx`/`ddy`) and `GetSunShadow` (filtered shadow-map sample) run once per
overlapping blade layer per pixel — the exact cost overdraw multiplies.

Change: compute the shadow term per-vertex instead. Grass blade quads are
low-poly (a handful of vertices per blade); per-vertex shadow evaluation
is visually indistinguishable on thin foliage geometry and moves this
cost from "scales with overdraw" to "scales with blade vertex count."

- In the VS: after computing `worldPos`, call the shadow-sampling
  functions there (they need `SHADOW_INVPROJ_REG`/`SHADOW_INVVIEW_REG` and
  the shadow map sampler — VS-side shadow map sampling requires
  `tex2Dlod`/vertex-texture-fetch support; SM3.0 supports this on
  vs_3_0 profile with `D3DUSAGE_DMAP`-style caveats — confirm the
  existing shadow map texture is created with vertex-texture-fetch
  compatible format/filtering. If VTF is unavailable in practice, fall
  back to computing shadow at coarse per-vertex granularity via a
  precomputed lower-res shadow term instead — this is the one open risk
  in this item, resolve before implementing (see Risks).
- Output a single `float sunShadow : TEXCOORD6` from VS to PS instead of
  `shadowWorldPos`.
- PS: replace `GetShadowGeometricNormal`/`GetSunShadow` calls with the
  interpolated `IN.sunShadow` scalar multiply — removes both the
  `ddx`/`ddy` call and the texture fetch from the pixel shader entirely.
- Keep `SkyAmbient`'s per-pixel normal input working off the same
  `shadowNormal` — if that also needs the geometric normal, compute it in
  VS too and pass it interpolated (cheap, no derivatives needed once
  computed per-vertex-in-VS instead of per-pixel-via-ddx).

### GO-2 — Alpha-test instead of alpha-blend, opaque depth write

**Files:** grass render-state setup (wherever vanilla currently sets
`D3DRS_ALPHABLENDENABLE`/`D3DRS_ALPHATESTENABLE` for the grass pass — not
found in NVR's own code, meaning it's native engine state; confirm exact
call site via a RenderDoc/PIX state capture before writing this item,
**do this first** since it determines whether this item is a state-value
change or a bigger pixel-shader-output change).

If grass currently blends: switch to `D3DRS_ALPHATESTENABLE = TRUE`,
`D3DRS_ALPHABLENDENABLE = FALSE`, `D3DRS_ZWRITEENABLE = TRUE`, pick an
`D3DRS_ALPHAREF`/`D3DRS_ALPHAFUNC` (e.g. `D3DCMP_GREATEREQUAL`, ref
~128) tuned against the current `OUT.color.a = saturate(albedo.a * 1.75f)
* IN.sun.w` output in the PS (`GRASS23x000TMS.pso.hlsl`). This lets
early-Z/hierarchical-Z reject fragments behind terrain or already-drawn
opaque grass before the pixel shader runs — the direct fix for
occluded-fragment overdraw. Requires grass to render after opaque
terrain/object depth is already written (confirm current pass ordering;
expected to already be true since grass sits visually on top of terrain).

Visual risk: alpha-test edges are harder-cut than blended edges. Compare
against current look; existing AA (SMAA, per `docs/` and prior branch
work) should soften the transition. Flag as a visual regression risk to
validate, not assume away.

### GO-3 — Front-to-back batch sort

**File:** `src/core/GrassInstancer.cpp` (`Flush()`, GI-2)

Before issuing the per-key instanced draws in `Flush()`, sort the
`frameAccumulator` keys by approximate camera distance (cheap: distance
from camera to the centroid or first instance's `posScale.xyz` per key)
and issue nearest-first. Maximizes how much of GO-2's early-Z rejection is
actually realized — nearer opaque-tested grass occludes farther grass in
the depth buffer before the farther batches are even shaded. Depends on
GO-2 (alpha-test + depth-write) to have any effect; no benefit while
still alpha-blending.

---

## Phase 3 — Distance/Angle Thinning (GT-1..GT-2)

Both are pure additions to `GrassInstancer::Flush()` (GI-2) — filtering
the instance list before upload, no shader changes.

### GT-1 — Distance-based instance decimation

Past a configurable distance threshold (new setting,
`[Shaders.Grass.Main] DecimateDistance`/`DecimateFactor`), drop every Nth
instance from `frameAccumulator[key]` before building the per-key vertex
buffer. Cuts overdraw at range from many overlapping thin blades without
any LOD/impostor system. Threshold should sit inside the existing
`GrassEndDistance` fade range so decimation and the existing distance fade
(`AlphaParam`-driven, `OUT.sun.w` in the VS) combine smoothly rather than
producing a visible density "step."

### GT-2 — Grazing-angle thinning

Grass viewed at a shallow angle stacks the most blade layers per screen
pixel. For each key, compute the angle between camera-forward and the
vector to that batch's approximate cell/cluster centroid; beyond a
configurable grazing threshold, apply additional decimation (compound
with GT-1, not a replacement for it). Same mechanism as GT-1, different
selection criterion — implement as a second filter pass over the same
per-key instance list in `Flush()`.

Both are cheap specifically because Phase 1 already puts the full,
CPU-accessible instance list in our hands before the GPU ever sees it —
this is the concrete payoff of doing instancing first.

---

## Risks / Open Questions

- **GO-1 vertex-texture-fetch availability.** Needs confirmation that the
  shadow map is sampleable from `vs_3_0` (VTF) on the hardware/driver
  matrix this project targets, particularly under DXVK (per this
  project's `CLAUDE.md` — DXVK translation quirks have already bitten
  input handling; verify they don't also affect VTF format/filter
  support before committing to GO-1's design).
- **GO-2 current render state unknown.** Must be captured via
  RenderDoc/PIX before writing code — plan assumes blend-based grass but
  this is unverified.
- **Instance data key stability.** `GrassBatchKey` uses raw D3D9 resource
  pointers (mesh/index/texture). Need to confirm the engine doesn't
  reuse/recreate these pointers across cell loads in a way that would
  silently merge unrelated grass types into one key, or thrash the
  dynamic-buffer pool. Watch for this in GI-5 validation.
- **Vertex declaration cost.** A new two-stream declaration is created
  once and reused — confirm no per-draw declaration churn is introduced
  by the flush path.
- **Interaction with `TheOcclusionManager`'s existing grass frustum
  culling** (`src/Oblivion/Hooks/Grass.cpp`, `UpdateGrassHook`) — that
  hook operates at cell level, before any of this; unaffected by this
  plan, but worth a regression check since it's the existing grass-perf
  code path this plan sits downstream of.

## Task Checklist

- [ ] GI-1 — Device wrapper capture hooks (`SetVertexShader`,
      `SetVertexShaderConstantF`, `DrawIndexedPrimitive`)
- [ ] GI-2 — `GrassInstancer` accumulator + `Flush()`
- [ ] GI-3 — Streamed VS variants (000S–003S) + vertex declaration
- [ ] GI-4 — `Instancing` setting + vanilla fallback path
- [ ] GI-5 — Validation (visual parity, instance-count parity, draw-call/perf capture)
- [ ] GO-1 — Shadow sampling moved VS-side (resolve VTF risk first)
- [ ] GO-2 — Alpha-test + depth-write (capture current state first)
- [ ] GO-3 — Front-to-back batch sort in `Flush()`
- [ ] GT-1 — Distance-based decimation
- [ ] GT-2 — Grazing-angle thinning
