#pragma once

class CombineDepthEffect : public EffectRecord
{
public:
	CombineDepthEffect() : EffectRecord("CombineDepth") {};

	HMODULE johnnyguitar = nullptr;
	bool(__cdecl* JG_SetClipDist)(float) = nullptr;
	float(__cdecl* JG_GetClipDist)() = nullptr;

	struct CombineDepthStruct {
		float viewNearZ;
	};
	CombineDepthStruct	Constants;

	struct CombineDepthTextures {
		IDirect3DTexture9* CombinedDepthTexture;
		IDirect3DSurface9* CombinedDepthSurface;
	};
	CombineDepthTextures	Textures;

	void	UpdateConstants();
	void	RegisterTextures();
	// Depth combine and normal reconstruction in one draw (two render targets). Returns false,
	// without rendering, when unavailable; the caller then runs the two passes separately.
	bool	RenderWithNormals(IDirect3DDevice9* Device, IDirect3DSurface9* NormalsSurface);
	bool	mergedNormalsFailed = false;
};