#pragma once

// UNOFFICIAL, optional (off by default; Shaders.Particles): lit particles and blood decals (GDECAL/GDECALS: blood on
// characters, unlit in the game too). The game draws blood, smoke, dust and
// debris particle systems unlit (BSSM_NOLIGHTING_PSYS and _SUBTEX_OFFSET: NOLIGHT016/017.vso + NOLIGHTTEXVC.pso),
// so they keep full brightness in shade, at night and indoors. The replacements light them with the ambient, the sun
// through the forward sun shadow, and nearby point lights. Glows, muzzle flashes, flames and sparks share the pixel
// shader but not the vertex shaders, and stay exactly as the game draws them (see SetShadersHook's pairing guard).
class ParticleShaders : public ShaderCollection
{
public:
	ParticleShaders() : ShaderCollection("Particles") {};

	struct ParticleStruct {
		D3DXVECTOR4		Data;		// x: strength (0 = as the game draws them), y: brightness, z: sun share, w: 1 outdoors (sun)
		D3DXVECTOR4		Ambient;	// rgb: ambient light (the sky's outdoors, the cell's indoors)
	};
	ParticleStruct	Constants;

	struct ParticleSettings {
		float	Strength;
		float	Brightness;
		float	SunShare;
	};
	ParticleSettings	Settings;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};
