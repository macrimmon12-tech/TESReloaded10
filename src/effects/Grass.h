#pragma once

class GrassShaders : public ShaderCollection
{
public:
	GrassShaders() : ShaderCollection("Grass") {};

	struct GrassStruct {
		D3DXVECTOR4		Scale;
		D3DXVECTOR4		Lighting;	// x: translucency, y: roundness, z: root darkening, w: specular
		D3DXVECTOR4		Lighting2;	// x: translucency focus, y: specular glossiness
	};
	GrassStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};