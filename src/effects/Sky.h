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
		// Order-2 spherical harmonic sky, 9 coefficients each, from one sweep.
		//
		// Irradiance is the upper hemisphere with nothing below, convolved with the clamped-cosine
		// kernel and divided by pi so it reconstructs as an average radiance - what the diffuse
		// ambient wants, since a surface collects the hemisphere weighted by the cosine.
		//
		// Radiance carries no convolution, so it reconstructs as radiance along a direction - what
		// the sky reflection wants. It is projected over the full sphere against the sky continued
		// below the horizon at its horizon colour: order 2 cannot represent the horizon's step, so
		// this set does not try, and the shader applies that edge itself (SkyHorizonVisibility).
		D3DXVECTOR4		Irradiance[9];
		D3DXVECTOR4		Radiance[9];
	};
	SkyStruct Constants;
	bool useSunDiskColor;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};