# Shader Tasks — To-Do

Tracking doc for a batch of shader work. Each item below is a placeholder —
details, design notes, and implementation plans will be filled in by
dedicated sessions per task. Check items off as they land.

## Tasks

- [ ] **Rain shader: player motion reactivity** — add reactivity to player
  movement (e.g. rain deflection/ripple response tied to player velocity).
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
