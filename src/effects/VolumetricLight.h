#pragma once

// Ray-marched sun light shaft, ported from arafuse/tes-reloaded's OblivionReloaded VolumetricLight
// effect and rewritten against this fork's VSM/EVSM cascade shadow atlas. Marches into its own
// half resolution buffer (mirrors FlashlightBeamEffect's pattern), then upsamples/combines onto
// the scene at full resolution.
class VolumetricLightEffect : public EffectRecord
{
public:
	VolumetricLightEffect() : EffectRecord("VolumetricLight") {
		Textures.VolumetricTexture = nullptr;
		Textures.VolumetricSurface = nullptr;
	};

	struct VolumetricLightSettingsStruct {
		float	Strength;
		float	FogDensity;
		float	Height;
		float	Anisotropy;
		bool	Dither;
		D3DXVECTOR3	ScatterColor;
		D3DXVECTOR3	WindDirection;
		float	AccumDistance;
	};
	VolumetricLightSettingsStruct	Settings;

	struct VolumetricLightStruct {
		D3DXVECTOR4	Data1;	// xyz: scatter color tint, w: accum distance cutoff
		D3DXVECTOR4	Data2;	// xyz: wind direction, w: unused
		D3DXVECTOR4	Data3;	// x: strength (intensity multiplier), y: fog density, z: height, w: anisotropy
		D3DXVECTOR4	Data4;	// x: unused, y: dither toggle
	};
	VolumetricLightStruct	Constants;

	struct VolumetricLightTextures {
		IDirect3DTexture9* VolumetricTexture;
		IDirect3DSurface9* VolumetricSurface;
	};
	VolumetricLightTextures	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
};
