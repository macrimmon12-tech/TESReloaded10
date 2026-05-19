#pragma once

class VolumetricFogEffect : public EffectRecord
{
public:
	VolumetricFogEffect() : EffectRecord("VolumetricFog") {};

	struct VolumetricFogStruct {
		D3DXVECTOR4		LowFog;
		D3DXVECTOR4		HighFog;
		D3DXVECTOR4		SimpleFog;
		D3DXVECTOR4		Blend;
		D3DXVECTOR4		Height;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Wind;            // xy: exterior scroll (windDir*windSpeed*baseSpeed), zw: interior drift
		D3DXVECTOR4		Noise;           // x: frequency, y: strength, z: sun attenuation, w: fog light strength
		D3DXVECTOR4		FogLight[2];     // xyz: world pos, w: radius
		D3DXVECTOR4		FogLightColor[2];// xyz: color, w: dimmer
		// C++-side settings, not passed to shader directly
		bool			UseWindDirection;
		float			WindBaseSpeed;
		float			WindFallbackDir;
		float			InteriorDriftSpeed;
		float			InteriorDriftDir;
	};
	VolumetricFogStruct	Constants;

	float	Amount;
	float	AmountInteriors;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();
};