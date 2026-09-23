#pragma once

// Temporal anti-aliasing, phase 1: temporal accumulation with camera reprojection, no sub-pixel
// jitter. Ported from Oblivion Reloaded E3's TAA; see TAA.fx.hlsl for what changed and why.
//
// Two passes per frame, both full resolution:
//   Resolve: current frame + reprojected history -> ResolveTexture (FP16)
//   copy:    ResolveTexture -> HistoryTexture, which becomes next frame's history
//   Output:  HistoryTexture -> the frame
// Resolve cannot write the history it is reading, hence the separate target. The history is FP16
// rather than the frame's 8 bits because at a 0.9 blend weight each frame moves the history only
// 10% of the way to the current value, and in 8 bits any difference under about 5/255 rounds
// straight back to the old value -- a ghost that stops fading and never clears.
class TAAEffect : public EffectRecord
{
public:
	TAAEffect() : EffectRecord("TAA") {};

	struct TAASettingsStruct {
		float	HistoryWeight;
		float	ClipGamma;
		int		DebugView;
	};
	TAASettingsStruct	Settings;

	struct TAAStruct {
		D3DXVECTOR4	Data;				// x: history weight, y: clip gamma, z: history valid (0/1), w: debug view (0 = off)
		D3DXVECTOR4	PrevProjection;		// x: last frame's projMatrix._11, y: its _22
		D3DXVECTOR4	CameraDelta;		// xyz: camera position this frame minus last frame's
		D3DXMATRIX	PrevViewTransform;	// last frame's viewMatrix; the shader reads only its rotation
	};
	TAAStruct	Constants;

	struct TAATexturesStruct {
		IDirect3DTexture9*	ResolveTexture = nullptr;
		IDirect3DSurface9*	ResolveSurface = nullptr;
		IDirect3DTexture9*	HistoryTexture = nullptr;
		IDirect3DSurface9*	HistorySurface = nullptr;
	};
	TAATexturesStruct	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();

	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer);

private:
	bool		historyValid = false;
	D3DXVECTOR4	prevCameraPosition = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);

	void	RememberCamera();
	bool	DrawTechnique(const char* TechniqueName);
};
