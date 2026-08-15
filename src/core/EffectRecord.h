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
	void					ClearSampler(const char* TextureName, size_t Length);
	void					DisposeEffect();
	bool					LoadEffect();

	bool 					IsLoaded();
	bool					Enabled;
	float					renderTime;
	float					constantUpdateTime;

	ID3DXEffect* Effect;
	const char* Name;

	// ---- R3: EffectRecord owns its own settings-menu rendering -------------
	// See docs/refactor-plan.md Refactor 3.

	// Renders this effect's settings for the already-selected leaf `section`
	// (e.g. "Shaders.Bloom.Main"). Default implementation renders generically
	// via RenderGenericSection() below, enriched by this effect's own HLSL <>
	// uniform annotations where present (R3d). Effects needing bespoke UI
	// (e.g. LUTEffect's day/night/interior texture pickers) override this.
	virtual void			RenderMenu(const char* section);

	// Optional hook for effects that need a side effect when a `path`-widget
	// setting commits a new filename (e.g. LUTEffect rebinding a texture
	// slot). Default is a no-op; `uniformName` is the TESR_-prefixed uniform
	// the path widget was bound to, `newPath` the filename just selected.
	virtual void			OnPathChanged(const char* uniformName, const char* newPath) {};

	// Generic settings-section renderer shared by every EffectRecord's default
	// RenderMenu() and by the settings overlay's fallback path for sections
	// that don't belong to an EffectRecord (ShaderCollection-backed shaders,
	// and non-shader settings panes like Main.*). Zero per-effect knowledge
	// lives in the caller -- `owner` is optional and, when supplied, is only
	// used to look up per-uniform annotations for widget enrichment.
	static void				RenderGenericSection(const char* section, bool isShaderSection, EffectRecord* owner = nullptr);

	// Clears the per-setting revert snapshot and colour-picker widget state.
	// Call when the overlay opens or settings are reloaded from disk.
	static void				ResetMenuState();

	// Re-applies every snapshotted value taken since the last ResetMenuState()
	// (i.e. "revert everything touched this session"), then clears menu state.
	static void				RevertMenuSnapshot();

	// Lazily records `section`.`key`'s current persisted value into the
	// revert snapshot, if it hasn't been recorded this session yet -- the
	// same lazy-seed RenderGenericSection() does for every setting it
	// renders, exposed for callers that mutate a setting through a path
	// RenderGenericSection() never sees. The one user today is the shader
	// enable/disable toggle: its setting lives in a `Status` section that's
	// deliberately hidden from the sidebar (see ShouldHideSection in
	// ImGuiManager.cpp), so it's never rendered, never lazily snapshotted by
	// RenderMenuNode, and previously never actually reverted by "Revert"
	// despite looking like it should.
	static void				SnapshotSettingIfUnseen(const char* section, const char* key);

	// ---- R3d: HLSL <> annotation parsing ------------------------------------

	enum class MenuWidget {
		Default, // type-default: slider for float(s), drag-int for int, checkbox for bool
		Color,   // RGB colour triple picker (float3)
		Enum,    // dropdown combo, requires EnumNames (int)
		Key,     // DIK keybind picker (int)
		Slider,  // explicit slider, overrides drag-int default (int)
		Path,    // file cycle picker (string)
		Hidden,  // uniform is skipped entirely when building a settings list (R3e/R3f-1)
		Packed,  // uniform packs N independent settings; see ComponentKeys etc. (R3f-1)
	};

	struct UniformAnnotation {
		std::string	Name;			// display label; falls back to the uniform's own name if omitted
		std::string	Description;	// tooltip text; empty means no tooltip
		MenuWidget	Widget = MenuWidget::Default;
		float		Min = 0.0f;
		float		Max = 1.0f;
		float		Step = 0.001f;
		std::string	Folder;			// required for Path widget
		std::string	Filter = "*.dds"; // optional for Path widget
		std::string	EnumNames;		// comma-delimited, required for Enum widget

		// ---- R3f-1: Packed widget (docs/refactor-plan.md "Packed Components") --
		// All comma-delimited, x/y/z/w order, parsed raw here (same convention as
		// EnumNames above) -- splitting/interpretation is the consumer's job
		// (R3f-2's per-effect uniform index), not this parser's. A component with
		// no real setting is simply absent from ComponentKeys, so it may be
		// shorter than the uniform has components. Only meaningful when
		// Widget == Packed; empty on every other uniform.
		std::string	ComponentKeys;		// SettingManager keys this uniform backs
		std::string	ComponentNames;		// optional; falls back to the key, per component
		std::string	ComponentDefaults;	// optional
		std::string	ComponentMins;		// optional
		std::string	ComponentMaxs;		// optional
		std::string	ComponentSteps;		// optional

		bool		HasDefault = false; // true only when the required `defaultValue` annotation was present and readable
	};

	// Reads the <> annotation block for the TESR_-prefixed uniform named
	// `uniformName` on this effect's constant table. Returns false (leaving
	// `out` untouched) if the uniform doesn't exist, carries no annotations,
	// or has no `defaultValue` field -- the one required field per the locked
	// annotation format (docs/refactor-plan.md R3a). Never throws; a
	// malformed annotation block degrades to "not found" rather than
	// crashing or partially populating `out`.
	bool					GetUniformAnnotation(const char* uniformName, UniformAnnotation* out);
};
