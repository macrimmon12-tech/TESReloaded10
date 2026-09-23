#pragma once

// Ray-marched sun light shaft: camera to visible surface, testing the sun's VSM/EVSM cascade
// shadow atlas at each step. A pixel only lights up where that shadow value varies along the
// ray -- i.e. an actual occluder -- there is no ambient fog/haze term. Marches into its own half
// resolution buffer (mirrors FlashlightBeamEffect's pattern), then upsamples/combines onto the
// scene at full resolution.
class VolumetricLightEffect : public EffectRecord
{
public:
	VolumetricLightEffect() : EffectRecord("VolumetricLight") {
		Textures.VolumetricTexture = nullptr;
		Textures.VolumetricSurface = nullptr;
	};

	struct VolumetricLightSettingsStruct {
		float	Strength;
		float	Anisotropy;
		bool	Dither;
		D3DXVECTOR3	ScatterColor;
		float	AccumDistance;
		float	ScatterReference;
		float	FogInfluence;
		float	Extinction;
		float	HeightFalloff;
		bool	DitherMotion;
	};
	VolumetricLightSettingsStruct	Settings;

	struct VolumetricLightStruct {
		D3DXVECTOR4	Data1;	// xyz: scatter color tint, w: reference path length / march range
		D3DXVECTOR4	Data3;	// x: strength, y: extinction, z: fog influence, w: anisotropy
		D3DXVECTOR4	Data4;	// x: scatter reference, y: dither, z: height falloff, w: dither motion
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
