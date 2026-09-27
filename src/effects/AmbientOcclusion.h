#pragma once

class AmbientOcclusionEffect : public EffectRecord
{
public:
	AmbientOcclusionEffect() : EffectRecord("AmbientOcclusion") {
		Constants.Fold = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		Textures.BufferTexture = nullptr;
		Textures.BufferSurface = nullptr;
		Textures.ScratchTexture = nullptr;
		Textures.ScratchSurface = nullptr;
	};

	bool bNVAOLoaded = false;

	struct AmbientOcclusionStruct {
		bool			Enabled;
		D3DXVECTOR4		AOData;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Fold;	// x: the exterior shadow composite applies TESR_AmbientOcclusionBuffer this frame
	};
	AmbientOcclusionStruct	Constants;

	struct AmbientOcclusionTextures {
		IDirect3DTexture9* BufferTexture;
		IDirect3DSurface9* BufferSurface;
		IDirect3DTexture9* ScratchTexture;
		IDirect3DSurface9* ScratchSurface;
	};
	AmbientOcclusionTextures	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();

	bool	RenderBuffers(IDirect3DDevice9* Device);
};