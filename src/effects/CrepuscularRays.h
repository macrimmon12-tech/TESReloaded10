#pragma once

// Crepuscular rays (sunbeams through gaps in occluders): real shadow-cascade-raymarched shafts,
// shaped by actual 3D occluders rather than a screen-space approximation. Replaces GodRays' own
// former "Enhanced"/"Volumetric" tiers -- see CrepuscularRays.fx.hlsl for why this lives as its
// own effect rather than a third GodRays technique. Selected via Shaders.GodRays.Main.Quality (0:
// Classic, 1: CrepuscularRays); ShaderManager decides which of GodRays/CrepuscularRays actually
// renders each frame based on that same setting.
class CrepuscularRaysEffect : public EffectRecord
{
public:
	CrepuscularRaysEffect() : EffectRecord("CrepuscularRays") {
		Textures.MarchTexture = nullptr;
		Textures.MarchSurface = nullptr;
	};

	struct CrepuscularRaysStruct {
		D3DXVECTOR4		Tint;  // xyz: scatter color tint, w: accum distance cutoff (march range)
		D3DXVECTOR4		Data;  // x: strength, y: unused, z: fog influence, w: anisotropy
		D3DXVECTOR4		Debug; // x: debug view mode, y: dither toggle (z, w unused)
	};
	CrepuscularRaysStruct	Constants;

	struct CrepuscularRaysTextures {
		IDirect3DTexture9* MarchTexture;
		IDirect3DSurface9* MarchSurface;
	};
	CrepuscularRaysTextures	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
};
