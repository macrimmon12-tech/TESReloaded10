#pragma once

class GrassShaders : public ShaderCollection
{
public:
	GrassShaders() : ShaderCollection("Grass") {};

	struct GrassStruct {
		D3DXVECTOR4		Scale;
		// Zeroed until the first UpdateSettings: all-zero grass lighting is vanilla grass.
		D3DXVECTOR4		Lighting = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// x: translucency, y: roundness, z: root darkening, w: specular
		D3DXVECTOR4		Lighting2 = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// x: translucency focus, y: specular glossiness, z: debug view, w: diffuse wrap
		D3DXVECTOR4		Lighting3 = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// x: root darkening height (units), y: point light strength, z: detail distance, w: detail fade
		D3DXVECTOR4		Lighting4 = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// x: shadow distance, y: shadow fade
	};
	GrassStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};