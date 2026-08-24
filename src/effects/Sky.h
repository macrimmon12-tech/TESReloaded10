#pragma once

class SkyShaders : public ShaderCollection
{
public:
	SkyShaders() : ShaderCollection("Sky") {};

	struct SettingsStruct{
		float SkyMultiplierDay;
		float SkyMultiplierNight;
	};
	SettingsStruct Settings;

	struct SkyStruct {
		D3DXVECTOR4		SkyData;
		D3DXVECTOR4		SunsetColor;
		D3DXVECTOR4		CloudData;
		// Order-2 spherical harmonic sky, 9 coefficients each, from one projection.
		//
		// Irradiance carries the clamped-cosine convolution, divided by pi so it reconstructs
		// as an average radiance -- what the diffuse ambient wants, since a surface collects
		// the hemisphere weighted by the cosine.
		//
		// Radiance is the same projection with that convolution divided back out, so it
		// reconstructs as radiance along a direction -- what a specular lobe wants, since it
		// asks what the sky LOOKS like that way rather than how much of it a surface sees.
		// Reconstructing the cosine-weighted set for a reflection reads a uniformly bright sky
		// as half as bright at the horizon as at the zenith.
		D3DXVECTOR4		Irradiance[9];
		D3DXVECTOR4		Radiance[9];
	};
	SkyStruct Constants;
	bool useSunDiskColor;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};