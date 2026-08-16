#pragma once

// In-air light shaft for the flashlight cone. Marches the view ray through the cone
// volume into a half resolution buffer; the Flashlight effect's Combine pass adds it.
class FlashlightBeamEffect : public EffectRecord
{
public:
	FlashlightBeamEffect() : EffectRecord("FlashlightBeam") {
		Constants.Control = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		Textures.VolumetricTexture = nullptr;
		Textures.VolumetricSurface = nullptr;
	};

	struct FlashlightBeamSettingsStruct {
		bool	FirstPerson;
		bool	ThirdPerson;
		float	Strength;
		float	Anisotropy;
		float	HeightFalloff;
		float	MaxDistance;
		float	FirstPersonStrength;
		float	FirstPersonOffsetRight;
		float	FirstPersonOffsetDown;
	};
	FlashlightBeamSettingsStruct	Settings;

	struct FlashlightBeamStruct {
		D3DXVECTOR4	Control;	// x strength (0 = off, also the Combine gate), y anisotropy, z height falloff, w max distance
	};
	FlashlightBeamStruct	Constants;

	struct FlashlightBeamTextures {
		IDirect3DTexture9* VolumetricTexture;
		IDirect3DSurface9* VolumetricSurface;
	};
	FlashlightBeamTextures	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
};
