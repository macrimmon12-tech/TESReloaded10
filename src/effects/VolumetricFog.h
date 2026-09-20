#pragma once

class VolumetricFogEffect : public EffectRecord
{
public:
	VolumetricFogEffect() : EffectRecord("VolumetricFog") {
		Constants.WeatherFilterAnimator.Initialize(1);
		weatherFilterWasActive = true;
		rainyDisablesSkyFilter = true;
		cloudyDisablesSkyFilter = true;
	};

	struct VolumetricFogStruct {
		D3DXVECTOR4		Density;         // x: BaseDensity, y: WeatherImpact, z: MorningFogDip, w: SunriseSunsetBoost
		D3DXVECTOR4		Shape;           // x: HeightFalloff, y: MaxHeight, z: Extinction, w: Inscattering
		D3DXVECTOR4		Wind;            // x: WindDirX, y: WindDirY, z: WindSpeed, w: NoiseScale
		D3DXVECTOR4		Scatter;         // x: PhaseAsymmetry, y: ShadowStrength, z: NoiseStrength, w: unused
		D3DXVECTOR4		Weather;         // x: WeatherFilterBlend (animated 0-1, 1=sky mask active), y: isExterior, z: SkyAmbientAvailable, w: FogSaturation
		D3DXVECTOR4		Aerial;          // x: AerialStrength, y: AerialRangeStart, z: AerialTintBlend, w: unused
		D3DXVECTOR4		AerialTintColor; // xyz: manual aerial tint override
		D3DXVECTOR4		Distant;         // x: DistantFogRange, y: DistantFogBlend, z: DistantFogHeight, w: EdgeAA
		D3DXVECTOR4		Global;          // x: Amount, y: unused, z: unused, w: unused
		Animator		WeatherFilterAnimator;
	};
	VolumetricFogStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();

	// cached from UpdateSettings, consumed by UpdateConstants to drive WeatherFilterAnimator
	bool	rainyDisablesSkyFilter;
	bool	cloudyDisablesSkyFilter;
	bool	weatherFilterWasActive; // edge-detect for the animator, mirrors RainAnimator.switched
};
