#pragma once

class SkyShaders : public ShaderCollection
{
public:
	// Zeroed, not left as whatever the allocation held: the coefficients are only computed while
	// the Sky shader is enabled, and Irradiance[0].w is the flag the shaders read to know that.
	SkyShaders() : ShaderCollection("Sky") { memset(&Constants, 0, sizeof(Constants)); };

	struct SettingsStruct{
		float SkyMultiplierDay;
		float SkyMultiplierNight;
	};
	SettingsStruct Settings;

	struct SkyStruct {
		D3DXVECTOR4		SkyData;
		D3DXVECTOR4		SunsetColor;
		D3DXVECTOR4		CloudData;
		// Order-2 spherical harmonic irradiance for the sky, for the ambient in the lighting
		// shaders. 9 coefficients, already convolved with the clamped-cosine kernel.
		D3DXVECTOR4		Irradiance[9];
	};
	SkyStruct Constants;
	bool useSunDiskColor;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};