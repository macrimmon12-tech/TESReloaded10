#pragma once

class ShadowsInteriorsEffect : public EffectRecord
{
public:
	ShadowsInteriorsEffect() : EffectRecord("ShadowsInteriors") {};

	struct ShadowsInteriorsStruct {
	};
	ShadowsInteriorsStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();

	// Blurs the shadow term on the two G16R16 scratch targets and touches the HDR frame only in the
	// final pass (see the DedicatedInteriorShadows technique); falls back to the generic path.
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
		UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	bool	dedicatedFailed = false;
};