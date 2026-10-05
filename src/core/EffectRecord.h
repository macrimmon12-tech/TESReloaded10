#pragma once

class EffectRecord : public ShaderProgram {
public:
	EffectRecord(const char* effectName);
	virtual ~EffectRecord();

	virtual void			SetCT();
	virtual void			CreateCT(ID3DXBuffer* ShaderSource, ID3DXConstantTable* ConstantTable);
	virtual void			UpdateConstants() {};
	virtual void			UpdateSettings() {};
	virtual void			RegisterConstants() {};
	virtual void			RegisterTextures() {};
	virtual bool			ShouldRender() { return true; }; // reimplement in subclasses to disable render under certain conditions
	virtual bool			SwitchEffect();
	virtual void			Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer);
	// ClearSampler is inherited from ShaderProgram -- game shaders need the same thing.
	void					DisposeEffect();
	bool					LoadEffect();

	bool 					IsLoaded();
	bool					Enabled;
	bool					usesSourceBuffer = true; // false when the effect declares no TESR_SourceBuffer sampler
	// True when a pass can leave destination pixels unwritten or depends on their contents
	// (clip/discard, blending, stencil, colour write masks). Under the frame chain such passes
	// get their destination pre-filled with the current image, matching the old in-place result.
	bool					needsPrefill = true;
	void					RebindSlotTextures();
	float					renderTime;
	float					constantUpdateTime;

	ID3DXEffect* Effect;
	const char* Name;
};
