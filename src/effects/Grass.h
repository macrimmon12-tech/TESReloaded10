#pragma once

class GrassShaders : public ShaderCollection
{
public:
	GrassShaders() : ShaderCollection("Grass") {};

	struct GrassStruct {
		D3DXVECTOR4		Scale;
		// Zeroed until the first UpdateSettings: all-zero grass lighting is vanilla grass.
		D3DXVECTOR4		Lighting = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// x: translucency, y: roundness, z: root darkening, w: specular
		D3DXVECTOR4		Lighting2 = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// x: translucency focus, y: specular glossiness, z: debug view
	};
	GrassStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};