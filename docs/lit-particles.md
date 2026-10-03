# Lit particles and blood decals

`Shaders.Particles.Status.Enabled` defaults to false. Enabling it lights supported blood, smoke, dust and debris particles and geometry blood decals using ambient light, nearby point lights and the sun through the forward shadow map.

`Shaders.Particles.Main` provides `Strength`, `Brightness` and `SunShare`. Strength zero restores the unlit colour multiplier within the replacement shaders. Disabling the collection selects the game's shaders.

Ambient and up to 24 point lights are evaluated per vertex; sun shadows are evaluated per pixel. This avoids a 24-light loop at every overlapping smoke or blood pixel. Interior ambient comes from the cell; outdoor ambient comes from the weather.

## Supported shaders and safeguards

- `NOLIGHT016` and `NOLIGHT017`: particle systems and subtexture-offset systems.
- `NOLIGHT006`: falloff cards, including blood spray. Lighting is applied only for normal fog/blend toggles; the fog toggles are a heuristic for distinguishing these cards from additive flashes.
- `NOLIGHTTEXVC`: texture multiplied by vertex colour, preserving the game's fog and alpha calculations.
- `GDECAL` / `GDECALS`: geometry blood decals, including four-bone skinned decals. The pixel shader preserves the original alpha times fade squared.

The pairing guard restores the vanilla pair when only one shader has a replacement, since D3D9 cannot pair SM3 replacements with the game's older shader models. First-person draws also use the vanilla pair. This upstream port tracks the first-person render scope directly; it does not require the optimized build's profiler.

Skinned decal camera matrices use c180/c184; particle lighting occupies c200-c250. The feature does not import the fork's sun crossfade, bounce light or contact-hardening changes.

## Validation and remaining checks

The Release/Win32 DLL builds, and all seven replacement shaders compile with D3DX9_43. The SDK compiler also passes all seven with forward shadows enabled and disabled. They also compile against mirjomi's shader includes at `6ec2eeb8`. The implementation is ported from adeesababa's optimized build; these compiler checks do not establish in-game compatibility with the other fork.

A fresh GPU test could not create a D3D9 device in this session (`0x8876086c`). Before release, check smoke and blood in daylight/shade, at night and in interiors; blood on animated actors; additive flashes and fire; first-person muzzle flashes and the Pip-Boy; and switching the collection off again. Test with both native D3D9 and DXVK. Large overlapping particles can still add measurable GPU work.
