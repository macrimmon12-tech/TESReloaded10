#pragma once

class VolumetricFogEffect : public EffectRecord
{
public:
	VolumetricFogEffect() : EffectRecord("VolumetricFog") {
		Constants.WeatherFilterAnimator.Initialize(1);
		weatherFilterWasActive = true;
		rainyDisablesSkyFilter = true;
		cloudyDisablesSkyFilter = true;

		Constants.HeroCrossfadeAnimator.Initialize(1);
		currentHeroLightIndex = -1;
		previousHeroLightIndex = -1;
		HeroCubeMapA = nullptr;
		HeroCubeMapB = nullptr;
		heroHysteresisMargin = 0.25f;
		heroCrossfadeDuration = 0.05f;
	};

	struct VolumetricFogStruct {
		D3DXVECTOR4		Density;         // x: BaseDensity, y: WeatherImpact, z: MorningFogDip, w: SunriseSunsetBoost
		D3DXVECTOR4		Shape;           // x: HeightFalloff, y: MaxHeight, z: Extinction, w: Inscattering
		D3DXVECTOR4		Wind;            // x: WindDirX, y: WindDirY, z: WindSpeed, w: NoiseScale
		D3DXVECTOR4		Scatter;         // x: PhaseAsymmetry, y: ShadowStrength, z: NoiseStrength, w: HeightInfluence
		D3DXVECTOR4		Weather;         // x: WeatherFilterBlend (animated 0-1, 1=sky mask active), y: isExterior, z: SkyAmbientAvailable, w: FogSaturation
		D3DXVECTOR4		Aerial;          // x: AerialStrength, y: AerialRangeStart, z: AerialTintBlend, w: unused
		D3DXVECTOR4		AerialTintColor; // xyz: manual aerial tint override
		D3DXVECTOR4		Distant;         // x: DistantFogRange, y: DistantFogBlend, z: DistantFogHeight, w: EdgeAA
		D3DXVECTOR4		Global;          // x: Amount, y: unused, z: unused, w: unused
		D3DXVECTOR4		HeroLightA;      // xyz: world position of the current hero point light, w: radius (0 = none)
		D3DXVECTOR4		HeroLightB;      // same, for the previous hero light (fading out during a crossfade)
		D3DXVECTOR4		HeroColorA;      // rgb: color, a: intensity, of the current hero light
		D3DXVECTOR4		HeroColorB;      // same, for the previous hero light
		D3DXVECTOR4		HeroBlend;       // x: crossfade blend (0=previous, 1=current), y: InteriorGlowStrength, z: InteriorShaftStrength, w: unused
		Animator		WeatherFilterAnimator;
		Animator		HeroCrossfadeAnimator;
	};
	VolumetricFogStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();

	// cached from UpdateSettings, consumed by UpdateConstants to drive WeatherFilterAnimator
	bool	rainyDisablesSkyFilter;
	bool	cloudyDisablesSkyFilter;
	bool	weatherFilterWasActive; // edge-detect for the animator, mirrors RainAnimator.switched

	// interior point-light hero selection: picked once per frame in C++ (see UpdateConstants),
	// not per-pixel -- SM3 cannot dynamically index the 12 separately-named shadow cubemap
	// samplers, so the choice has to be made here and the result rebound through dedicated
	// sampler slots (see RegisterConstants/HeroCubeMapA/B).
	int		currentHeroLightIndex;
	int		previousHeroLightIndex;
	float	heroHysteresisMargin;
	float	heroCrossfadeDuration; // Animator units are GAME HOURS, not seconds -- see UpdateConstants

	// Dynamically rebound every frame to whichever light currently wins (see UpdateConstants).
	// Registered once via TextureManager::RegisterTexture, which stores the ADDRESS of these
	// pointers; ClearSampler must still be called every frame to force EffectRecord::SetCT to
	// re-resolve through them, since it only calls BindTexture when its cached pointer is null.
	IDirect3DCubeTexture9*	HeroCubeMapA;
	IDirect3DCubeTexture9*	HeroCubeMapB;
};
