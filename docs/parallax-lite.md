# ParallaxLite

Enable `Main.Main.ReducedQuality.ParallaxLite` in the menu or configuration. It defaults to false and applies immediately.

The option covers terrain and parallax-mapped objects. It uses up to eight coarse search steps followed by one secant correction instead of contact refinement. Terrain also uses two parallax-shadow taps and caps its configured parallax distance at `1024 * screen height / 1440`. A shorter user-configured distance still applies. Object parallax keeps its existing distance fade and shadow taps.

This trades some accuracy for speed. Check grazing angles, steep height maps and terrain transitions. It overrides terrain `HighQuality` while enabled; disabling it restores the configured quality and distance.

The implementation comes from adeesababa's optimized build, commits `40145925` and `0c64bbc1`. The separate object search avoids folding the cheaper search into the existing refinement loop. No configuration migration from that fork is needed for upstream.

## Validation

- Release/Win32 DLL builds with VS2019 v142.
- All 53 POM template variants and 14 terrain pixel variants (one to seven textures, with and without 24 point lights) compile with D3DX9_43.
- The shared parallax include is identical between upstream `ef0fc5d5` and mirjomi's `vheyes-unofficial-45` at `6ec2eeb8`. Applying the new include to that branch's shaders compiles for the tested terrain and object variants. This is a source/compiler compatibility check, not an in-game compatibility claim.
- A fresh GPU pixel/timing comparison could not run: D3D9 device creation returned `0x8876086c`. Earlier measurements from the optimized build are not measurements of this port.

Before release, compare enabled/disabled views of terrain and parallax objects, including interiors, at near and grazing angles. Check that disabling the option restores normal rendering and that a user-configured terrain distance below the cap is preserved.
