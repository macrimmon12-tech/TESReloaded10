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

	// Field names/layout here are ours; the registered constant NAMES below (TESR_VolumetricLightData1/3/4)
	// match CrepuscularRays.fx.hlsl verbatim -- that file is copied in unmodified from a known-working
	// reference implementation, so its internal naming is left exactly as-is rather than renamed to fit
	// this codebase's usual TESR_<EffectName>* convention, to avoid any chance of a transcription bug.
	struct CrepuscularRaysStruct {
		D3DXVECTOR4		Tint;  // -> TESR_VolumetricLightData1. xyz: scatter color tint, w: accum distance cutoff (march range)
		D3DXVECTOR4		Data;  // -> TESR_VolumetricLightData3. x: strength, y: unused, z: fog influence, w: anisotropy
		D3DXVECTOR4		Debug; // -> TESR_VolumetricLightData4. x: debug view mode, y: dither toggle (z, w unused)
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
