#pragma once

class AmbientOcclusionEffect : public EffectRecord
{
public:
	AmbientOcclusionEffect() : EffectRecord("AmbientOcclusion") {};

	bool bNVAOLoaded = false;
	bool packedAOFailed = false;
	// Composite apply (set by ShaderManager around one Render call):
	// deferCombine -- run estimate and blurs only, leaving the result in ResultTexture() for the
	//                 fog pass to apply; deferredReady reports that it did.
	// combineOnly  -- run just the combine from ResultTexture() (fallback if the fog pass could not).
	bool deferCombine = false;
	bool deferredReady = false;
	bool combineOnly = false;
	IDirect3DTexture9* aoTexture[2] = {};
	IDirect3DSurface9* aoSurface[2] = {};
	// Quarter-resolution pair, used instead when [Main.Main.ReducedQuality] AOLowRes is on.
	IDirect3DTexture9* aoTextureLow[2] = {};
	IDirect3DSurface9* aoSurfaceLow[2] = {};
	// Which pair the last estimate wrote; combineOnly and the fog composite must read the same one.
	bool lastLowRes = false;

	bool	UseLowRes() const;
	IDirect3DSurface9* NextResultSurface() const { return UseLowRes() ? aoSurfaceLow[0] : aoSurface[0]; }
	IDirect3DTexture9* ResultTexture() const { return lastLowRes ? aoTextureLow[0] : aoTexture[0]; }

	struct AmbientOcclusionStruct {
		bool			Enabled;
		D3DXVECTOR4		AOData;
		D3DXVECTOR4		Data;
	};
	AmbientOcclusionStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	void Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
		IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
		IDirect3DSurface9* SourceBuffer) override;
};
