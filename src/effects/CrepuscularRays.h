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
	// were kept from the reference implementation this started as a verbatim copy of, rather than
	// renamed to this codebase's usual TESR_<EffectName>* convention, so a future diff against that
	// source stays legible. The shader's own internals have since diverged from that copy (darkens
	// occluded pixels instead of brightening lit ones -- see CrepuscularRays.fx.hlsl's header), which
	// is why xyz/z/w below are now unused rather than a tint/fog-influence/anisotropy.
	struct CrepuscularRaysStruct {
		D3DXVECTOR4		Tint;  // -> TESR_VolumetricLightData1. xyz: unused. w: accum distance cutoff (march range)
		D3DXVECTOR4		Data;  // -> TESR_VolumetricLightData3. x: darkening strength. y, z, w: unused
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
