#include <unordered_set>
#include "imgui.h"
#include "imgui_internal.h" // GetActiveID() (RenderMenuNode's text-field editing state) is internal-only
#include "ImGuiWidgets.h"

/*
* Class that wraps an effect shader, in order to load it/render it/set constants.
*/
EffectRecord::EffectRecord(const char* effectName) {

	Name = effectName;

	Effect = NULL;
	Enabled = false;
	renderTime = 0;
	constantUpdateTime = 0;
}

/*Shader Values arrays are freed in the superclass Destructor*/
EffectRecord::~EffectRecord() {
	if (Effect) Effect->Release();
}

/*
 * Unbinds all textures for samplers.
 * Useful when recreating textures on the fly.
 */
void EffectRecord::ClearSampler(const char* TextureName, size_t Length) {
	ShaderTextureValue* Sampler;
	for (UInt32 c = 0; c < TextureShaderValuesCount; c++) {
		Sampler = &TextureShaderValues[c];
		if (!memcmp(Sampler->Name, TextureName, Length) && Sampler->Texture->Texture) {
			Sampler->Texture->Texture = nullptr;
		}
	}
}

/*
 * Unload effects, allowing it to be reloaded from  a blank state.
 */
void EffectRecord::DisposeEffect() {
	if (Effect) Effect->Release();
	Effect = nullptr;

	delete[] FloatShaderValues;
	FloatShaderValues = nullptr;
	FloatShaderValuesCount = 0;

	delete[] TextureShaderValues;
	TextureShaderValues = nullptr;
	TextureShaderValuesCount = 0;

	PackedSettings.clear(); // R3f-2 -- stale once Effect (and its constant table) is gone

	Enabled = false;
}

/*
 * Compile and Load the Effect shader
 */
bool EffectRecord::LoadEffect() {
	auto timer = TimeLogger();
	bool success = false;

	ID3DXEffect* Effect = NULL;
	ID3DXEffectCompiler* Compiler = NULL;
	ID3DXBuffer* EffectSource = NULL;
	ID3DXBuffer* Errors = NULL;
	ID3DXBuffer* EffectBuffer = NULL;

	char BaseDirectory[MAX_PATH];
	char CacheDirectory[MAX_PATH];
	char EffectSourcePath[MAX_PATH];
	char EffectPreprocessedPath[MAX_PATH];
	char EffectCompiledPath[MAX_PATH];

	// Build filenames.
	strcpy(BaseDirectory, EffectsPath);
	strcpy(CacheDirectory, BaseDirectory);
	strcat(CacheDirectory, "Cache\\");

	strcpy(EffectSourcePath, BaseDirectory);
	strcat(EffectSourcePath, Name);
	strcat(EffectSourcePath, ".fx.hlsl");

	if (!FileExists(EffectSourcePath)) {
		return success;
	}

	strcpy(EffectPreprocessedPath, CacheDirectory);
	strcat(EffectPreprocessedPath, Name);
	strcat(EffectPreprocessedPath, ".fx.hlsl");

	strcpy(EffectCompiledPath, CacheDirectory);
	strcat(EffectCompiledPath, Name);
	strcat(EffectCompiledPath, ".fx");

	HRESULT prepass = D3DXPreprocessShaderFromFileA(EffectSourcePath, NULL, NULL, &EffectSource, &Errors);
	if (prepass == D3D_OK) {
		bool Compile = !CheckPreprocessResult(EffectPreprocessedPath, EffectSource);
		bool CompiledExists = FileExists(EffectCompiledPath);

		if (!Compile && CompiledExists) {
			HRESULT loaded = D3DXCreateEffectFromFileA(TheRenderManager->device, EffectCompiledPath, NULL, NULL, D3DXFX_LARGEADDRESSAWARE, NULL, &Effect, &Errors);
			if (FAILED(loaded)) {
				ReportError(loaded);
				if (Errors) Logger::Log((char*)Errors->GetBufferPointer());

				if (Effect) { Effect->Release(); Effect = NULL; }
				if (Errors) { Errors->Release(); Errors = NULL; }
			}

			if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
			
			success = true;
		}

		if (Compile || !CompiledExists || !Effect) {
			// compile if option was enabled or compiled version not found
			HRESULT compiled = D3DXCreateEffectCompiler(
				(const char*)EffectSource->GetBufferPointer(), 
				EffectSource->GetBufferSize(), 
				NULL, 
				NULL, 
				NULL, 
				&Compiler, 
				&Errors
			);
			if (FAILED(compiled)) {
				ReportError(compiled);
				if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
				if (Compiler) Compiler->Release();
				if (EffectSource) EffectSource->Release();
				if (Effect) Effect->Release();
				if (Errors) Errors->Release();
				Enabled = false;
				return success;
			}
			if (Errors) Logger::Log((char*)Errors->GetBufferPointer());

			compiled = Compiler->CompileEffect(NULL, &EffectBuffer, &Errors);
			if (FAILED(compiled)) {
				ReportError(compiled);
				if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
				if (Compiler) Compiler->Release();
				if (EffectSource) EffectSource->Release();
				if (Effect) Effect->Release();
				if (Errors) Errors->Release();
				Enabled = false;
				return success;
			}
			if (Errors) Logger::Log((char*)Errors->GetBufferPointer());

			if (EffectBuffer) {
				std::ofstream FileBinary(EffectCompiledPath, std::ios::out | std::ios::binary);
				FileBinary.write((const char*)EffectBuffer->GetBufferPointer(), EffectBuffer->GetBufferSize());
				FileBinary.flush();
				FileBinary.close();

				std::ofstream FilePreprocess(EffectPreprocessedPath, std::ios::out | std::ios::binary);
				FilePreprocess.write((const char*)EffectSource->GetBufferPointer(), EffectSource->GetBufferSize());
				FilePreprocess.flush();
				FilePreprocess.close();

				Logger::Log("Effect compiled: %s", EffectPreprocessedPath);
			}

			HRESULT loaded = D3DXCreateEffectFromFileA(TheRenderManager->device, EffectCompiledPath, NULL, NULL, D3DXFX_LARGEADDRESSAWARE, NULL, &Effect, &Errors);
			if (FAILED(loaded)) {
				ReportError(loaded);
				if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
				if (Compiler) Compiler->Release();
				if (EffectSource) EffectSource->Release();
				if (Effect) Effect->Release();
				if (Errors) Errors->Release();
				if (EffectBuffer) EffectBuffer->Release();
				Enabled = false;
				return success;
			}
			if (Errors) Logger::Log((char*)Errors->GetBufferPointer());

			success = true;
		}
	}
	else {
		ReportError(prepass);
		if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
	}

	if (Effect) {
		this->Effect = Effect;
		CreateCT(EffectSource, NULL); //Create the object which will associate a register index to a float pointer for constants updates;
		BuildPackedSettingsIndex(); // R3f-2 -- reads <> annotations, stays in step with CreateCT()'s own lifetime
		Logger::Log("Effect loaded: %s", EffectCompiledPath);
	}

	// set enabled status of effect based on success and setting
	Enabled = Effect != nullptr && TheSettingManager->GetMenuShaderEnabled(Name);

	if (Compiler) Compiler->Release();
	if (Errors) Errors->Release();
	if (EffectBuffer) EffectBuffer->Release();
	if (EffectSource) EffectSource->Release();

	timer.LogTime("EffectRecord::LoadEffect");

	return success;
}


/**
* Loads an effect shader by name (The post process effects stored in the Effects folder)
* @param Name the name for the effect
* @returns an EffectRecord describing the effect shader.
*/
bool EffectRecord::IsLoaded() {
	return Effect != nullptr;
}


/**
Creates the Constant Table for the Effect Record.
*/
void EffectRecord::CreateCT(ID3DXBuffer* ShaderSource, ID3DXConstantTable* ConstantTable) {
	auto timer = TimeLogger();

	D3DXEFFECT_DESC ConstantTableDesc;
	D3DXPARAMETER_DESC ConstantDesc;
	D3DXHANDLE Handle;
	UINT ConstantCount = 1;
	UInt32 FloatIndex = 0;
	UInt32 TextureIndex = 0;

	Effect->GetDesc(&ConstantTableDesc);
	for (UINT c = 0; c < ConstantTableDesc.Parameters; c++) {
		Handle = Effect->GetParameter(NULL, c);
		Effect->GetParameterDesc(Handle, &ConstantDesc);
		if (memcmp(ConstantDesc.Name, "TESR_", 5)) continue;
		if ((ConstantDesc.Class == D3DXPC_VECTOR || ConstantDesc.Class == D3DXPC_MATRIX_ROWS)) FloatShaderValuesCount += 1;
		if (ConstantDesc.Class == D3DXPC_OBJECT && ConstantDesc.Type >= D3DXPT_SAMPLER && ConstantDesc.Type <= D3DXPT_SAMPLERCUBE) TextureShaderValuesCount += 1;
	}
	FloatShaderValues = new ShaderFloatValue[FloatShaderValuesCount];
	TextureShaderValues = new ShaderTextureValue[TextureShaderValuesCount];

	//Logger::Debug("CreateCT: Effect has %i constants", ConstantTableDesc.Parameters);

	for (UINT c = 0; c < ConstantTableDesc.Parameters; c++) {
		Handle = Effect->GetParameter(NULL, c);
		Effect->GetParameterDesc(Handle, &ConstantDesc);
		if (memcmp(ConstantDesc.Name, "TESR_", 5)) continue; // Only treat constants named with TESR prefix

		switch (ConstantDesc.Class) {
		case D3DXPC_VECTOR:
			FloatShaderValues[FloatIndex].Name = ConstantDesc.Name;
			FloatShaderValues[FloatIndex].Type = (D3DXPARAMETER_TYPE)D3DXPC_VECTOR;
			FloatShaderValues[FloatIndex].RegisterIndex = (UInt32)Handle;
			FloatShaderValues[FloatIndex].RegisterCount = ConstantDesc.Elements;
			FloatIndex++;
			break;
		case D3DXPC_MATRIX_ROWS:
			FloatShaderValues[FloatIndex].Name = ConstantDesc.Name;
			FloatShaderValues[FloatIndex].Type = (D3DXPARAMETER_TYPE)D3DXPC_MATRIX_ROWS;
			FloatShaderValues[FloatIndex].RegisterIndex = (UInt32)Handle;
			FloatShaderValues[FloatIndex].RegisterCount = ConstantDesc.Elements;
			FloatIndex++;
			break;
		case D3DXPC_OBJECT:
			if (ConstantDesc.Class == D3DXPC_OBJECT && ConstantDesc.Type >= D3DXPT_SAMPLER && ConstantDesc.Type <= D3DXPT_SAMPLERCUBE) {
				TextureShaderValues[TextureIndex].Name = ConstantDesc.Name;
				TextureShaderValues[TextureIndex].Type = TextureRecord::GetTextureType(ConstantDesc.Type);
				TextureShaderValues[TextureIndex].RegisterIndex = TextureIndex;
				TextureShaderValues[TextureIndex].RegisterCount = 1;
				TextureShaderValues[TextureIndex].GetSamplerStateString(ShaderSource, TextureIndex);
				TextureShaderValues[TextureIndex].GetTextureRecord();

				TextureIndex++;
			}
			break;
		}
	}

	timer.LogTime("EffectRecord::ConstantTableDesc");
}

/*
*Sets the Effect Shader constants table and texture registers.
*/
void EffectRecord::SetCT() {
	if (!Enabled || Effect == nullptr) return;

	ShaderTextureValue* Sampler;
	for (UInt32 c = 0; c < TextureShaderValuesCount; c++) {
		Sampler = &TextureShaderValues[c];
		if (!Sampler->Texture->Texture) {
			Sampler->Texture->BindTexture(Sampler->Name);

			if (Sampler->Texture->Texture) Logger::Log("%s : Texture %s Succesfully bound", Name, Sampler->Name);
			//else Logger::Log("[ERROR] : Could not bind texture %s for effect %s", Sampler->Name, Name);
		}

		if (Sampler->Texture->Texture != nullptr) {
			TheRenderManager->device->SetTexture(Sampler->RegisterIndex, Sampler->Texture->Texture);
			for (int i = 1; i < SamplerStatesMax; i++) {
				TheRenderManager->SetSamplerState(Sampler->RegisterIndex, (D3DSAMPLERSTATETYPE)i, Sampler->Texture->SamplerStates[i]);
			}
		}
	}

	ShaderFloatValue* Constant;
	for (UInt32 c = 0; c < FloatShaderValuesCount; c++) {
		Constant = &FloatShaderValues[c];
		if (Constant->Value == nullptr)  Constant->GetValueFromConstantTable();
		if (Constant->Value == nullptr) {
			Logger::Log("[Error] %s : Couldn't get value for Constant %s", Name, Constant->Name);
			continue;
		}

		if (Constant->Type == (D3DXPARAMETER_TYPE)D3DXPC_VECTOR) {
			if (Constant->RegisterCount > 1)
				Effect->SetVectorArray((D3DXHANDLE)Constant->RegisterIndex, Constant->Value, Constant->RegisterCount);
			else
				Effect->SetVector((D3DXHANDLE)Constant->RegisterIndex, Constant->Value);
		}
		else
			if (Constant->RegisterCount > 1)
				Effect->SetMatrixArray((D3DXHANDLE)Constant->RegisterIndex, (D3DXMATRIX*)Constant->Value, Constant->RegisterCount);
			else
				Effect->SetMatrix((D3DXHANDLE)Constant->RegisterIndex, (D3DXMATRIX*)Constant->Value);
	}
}

/*
 * Enable or Disable Effect, with loading if not loaded
 */
bool EffectRecord::SwitchEffect() {
	bool change = true;
	if (!IsLoaded() && !Enabled) {
		Logger::Log("Effect %s is not loaded", Name);
		DisposeEffect();
		change = LoadEffect();
	}
	if (change) {
		Enabled = !Enabled;
	}
	else {
		char Message[256] = "Error: Couldn't enable effect ";
		strcat(Message, Name);

		InterfaceManager->ShowMessage(Message);
		Logger::Log(Message);
	}
	return Enabled;
}


/**
* Renders the given effect shader.
*/
void EffectRecord::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {

	if (!Enabled || Effect == nullptr || !ShouldRender()) {
		renderTime = 0.0f;
		return; // skip rendering of disabled effects
	}

	auto timer = TimeLogger();
	if (SourceBuffer) Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_LINEAR);

	try {
		D3DXHANDLE technique = Effect->GetTechnique(techniqueIndex);
		Effect->SetTechnique(technique);
		SetCT(); // update the constant table
		UINT Passes;
		Effect->Begin(&Passes, NULL);
		for (UINT p = 0; p < Passes; p++) {
			if (ClearRenderTarget) Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0L);
			Effect->BeginPass(p);
			Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Effect->EndPass();
			if (RenderedSurface) Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_LINEAR); // copy the result from the pass into the texture
		}
		Effect->End();
	}
	catch (const std::exception& e) {
		Logger::Log("Error during rendering of effect %s: %s", Name, e.what());
	}

	std::string name = "EffectRecord::Render " + std::string(Name);
	renderTime = timer.LogTime(name.c_str());
}

/*
 * ---- R3: EffectRecord owns its own settings-menu rendering -----------------
 * See docs/refactor-plan.md Refactor 3. RenderGenericSection() is the shared
 * per-node renderer used both by EffectRecord::RenderMenu()'s default
 * implementation and, via the `owner = nullptr` overload, by the settings
 * overlay's fallback path for sections that don't belong to an EffectRecord
 * (ShaderCollection-backed shaders, and non-shader settings panes).
 */

// Per-setting revert snapshot: section -> key -> value at the last
// ResetMenuState() call (overlay open / settings reload from disk).
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_menuSnapshot;

// Persistent hue/intensity decomposition for RGB-triple widgets, keyed by
// "<section>.<prefix>". Cleared alongside s_menuSnapshot.
static std::unordered_map<std::string, ImGuiWidgets::ColorTripleState> s_menuColorStates;
static bool s_menuColorStatesNeedReset = false;

static bool ShouldHideMenuKey(const char* key) {
	if (strncmp(key, "TextColor", 9) == 0 || strncmp(key, "TextShadow", 10) == 0) return true;
	// LUT filenames are rendered as cycle pickers, not raw text fields.
	if (strcmp(key, "DayLUT") == 0 || strcmp(key, "NightLUT") == 0 || strcmp(key, "InteriorLUT") == 0) return true;
	return false;
}

// R3f-2: splits a comma-delimited annotation string (componentKeys and
// friends -- same convention as enumNames) into its parts, preserving empty
// entries positionally (so e.g. "A,,B" yields {"A", "", "B"}, not {"A", "B"})
// -- callers that align several of these lists by position depend on that.
// An empty or all-comma input yields an empty vector rather than a vector of
// empty strings.
static std::vector<std::string> SplitCsv(const std::string& csv) {
	std::vector<std::string> parts;
	if (csv.empty()) return parts;
	std::stringstream ss(csv);
	std::string item;
	while (std::getline(ss, item, ',')) parts.push_back(item);
	return parts;
}

static void RenderMenuNode(SettingManager::Configuration::ConfigNode& node, bool isShaderSection, EffectRecord* owner) {
	using NodeType = SettingManager::Configuration::NodeType;

	ImGui::PushID(node.Key);

	auto& sectionSnap = s_menuSnapshot[node.Section];
	auto  snapIt      = sectionSnap.find(node.Key);
	if (snapIt == sectionSnap.end())
		snapIt = sectionSnap.emplace(node.Key, node.Value).first;
	bool isDirty = snapIt->second != std::string(node.Value);
	auto revert  = [&]() {
		TheSettingManager->SetSettingS(node.Section, node.Key, const_cast<char*>(snapIt->second.c_str()));
		TheSettingManager->LoadSettings();
	};

	// R3d: look up this key's <> annotation, if `owner` has a TESR_-prefixed
	// uniform of the same name (convention: "TESR_" + effect name + key,
	// matching the existing TESR_BloomData / TESR_ColoringColorCurve style).
	// Gracefully absent for settings with no shader-side counterpart.
	std::string uniformName;
	EffectRecord::UniformAnnotation ann;
	bool hasAnn = false;
	if (owner) {
		uniformName = std::string("TESR_") + owner->Name + node.Key;
		hasAnn = owner->GetUniformAnnotation(uniformName.c_str(), &ann);
		if (hasAnn && ann.Widget == EffectRecord::MenuWidget::Hidden) {
			// Explicitly marked hidden (R3e/R3f-1) -- render un-annotated,
			// same as "no annotation found", not enriched by it.
			hasAnn = false;
		}
	}

	// R3f-2: the direct convention above only ever matches a uniform that
	// *is* one setting (TESR_BloomFinalGain) or *is* one widget in its own
	// right (an RGB triple with widget=color). Almost every real setting is
	// packed 2-4 to a uniform instead -- TESR_BloomData.z is "PassBlending",
	// not "TESR_BloomPassBlending". When the direct lookup found nothing (or
	// found something explicitly hidden), fall back to this effect's
	// packed-uniform index (EffectRecord::BuildPackedSettingsIndex(),
	// docs/refactor-plan.md "Packed Components"). Synthesizing a normal `ann`
	// here rather than branching separately means every widget/tooltip/
	// revert code path below is unchanged -- only where the metadata came
	// from differs.
	if (!hasAnn && owner) {
		auto it = owner->PackedSettings.find(node.Key);
		if (it != owner->PackedSettings.end()) {
			ann = EffectRecord::UniformAnnotation{};
			ann.Name = it->second.Name;
			ann.Min  = it->second.Min;
			ann.Max  = it->second.Max;
			ann.Step = it->second.Step;
			ann.HasDefault = true;
			hasAnn = true;
			// Packed components have no unregistered-constant story of their
			// own: their owning packed uniform is always a real
			// RegisterConstants() entry whose UpdateConstants() already
			// pushes every frame via the normal SetCT() path. Clearing
			// uniformName makes syncLiveConstant() below a no-op here instead
			// of guessing a "TESR_"+Name+Key string that names no real
			// uniform (harmless either way -- SetCustomConstant() is a no-op
			// for an unrecognised name -- but this is the honest signal).
			uniformName.clear();
		}
	}

	const char* label   = (hasAnn && !ann.Name.empty()) ? ann.Name.c_str() : node.Key;
	const char* tooltip = (hasAnn && !ann.Description.empty()) ? ann.Description.c_str()
	                     : (node.Description.empty() ? nullptr : node.Description.c_str());

	// A setting with a matching annotation but no C++-side RegisterConstants()
	// entry (the "no C++ required" custom-effect path this demonstrates -- see
	// docs/refactor-plan.md R4) has nothing else keeping its live shader
	// constant in sync with the persisted value: no UpdateConstants() override
	// runs for it. Push the current value every time this node renders (not
	// just on edit) so the constant tracks the setting for as long as this
	// panel is open. TheShaderManager::SetCustomConstant() is a no-op until
	// the effect has rendered at least one frame with this uniform referenced
	// (which is what first creates its CustomConst slot); before that -- or
	// before the user ever opens this panel -- the shader-side value is
	// whatever the shader's own annotated `defaultValue` implies, which annotated
	// shaders are expected to fall back to via a guard (see Bloom.fx.hlsl's
	// TESR_BloomFinalGain for the pattern this smoke-tests). Guarded on
	// uniformName being non-empty so packed-component-sourced nodes (which
	// already have a live C++ UpdateConstants() path -- see above) never
	// take this path.
	auto syncLiveConstant = [&](float x) {
		if (hasAnn && !uniformName.empty()) TheShaderManager->SetCustomConstant(uniformName.c_str(), D3DXVECTOR4(x, 0.0f, 0.0f, 0.0f));
	};

	switch (node.Type) {
	case NodeType::Boolean: {
		bool val = node.BoolValue;
		if (ImGuiWidgets::Checkbox(label, &val)) {
			TheSettingManager->SetSetting(node.Section, node.Key, val);
			TheSettingManager->LoadSettings();
		}
		syncLiveConstant(val ? 1.0f : 0.0f);
		ImGuiWidgets::TooltipIfHovered(ImGui::IsItemHovered(), tooltip);
		if (ImGuiWidgets::RevertButton(isDirty)) revert();
		break;
	}
	case NodeType::Float: {
		float val = node.FloatValue;
		// Un-annotated (today, every shipped setting): fixed fine-drag speed,
		// global user-configurable quick-adjust step -- matches pre-R3 behavior.
		float minV = 0.0f, maxV = 0.0f, dragStep = 0.001f, quickStep = ImGuiWidgets::GetDefaultStep();
		if (hasAnn) {
			// Annotated: the uniform's own `step` controls both, per
			// docs/refactor-plan.md ("step controls both drag speed and the
			// +/- button increment").
			minV = ann.Min; maxV = ann.Max; dragStep = quickStep = ann.Step;
		}
		bool hovered = false;
		if (ImGuiWidgets::FloatSlider(label, &val, minV, maxV, dragStep, quickStep, isShaderSection, &hovered)) {
			TheSettingManager->SetSetting(node.Section, node.Key, val);
			TheSettingManager->LoadSettings();
		}
		syncLiveConstant(val);
		ImGuiWidgets::TooltipIfHovered(hovered, tooltip);
		if (ImGuiWidgets::RevertButton(isDirty)) revert();
		break;
	}
	case NodeType::Integer: {
		int  val     = node.IntValue;
		int  newVal  = val;
		bool changed = false;
		bool hovered = false; // set inside whichever branch below actually runs

		// Pre-existing hardcoded special case for the Tonemapping mode combo.
		// TonemappingShaders is a ShaderCollection -- it has no accessible
		// constant table to annotate against (see docs/refactor-plan.md,
		// "EffectRecord vs ShaderCollection"), so this can't become an
		// annotation-driven `enum` widget without a hand-written override.
		// Ported unchanged from the pre-R3 implementation.
		if (strcmp(node.Key, "TonemappingMode") == 0) {
			static const std::vector<std::string> kNames = {
				"0 - None (vanilla)", "1 - VTLottes", "2 - NVRLottes",
				"3 - Reinhard", "4 - Reinhard Jodie", "5 - ACES Filmic",
				"6 - ACES Fitted", "7 - Uncharted 2", "8 - Uchimura (GT)",
				"9 - AGX", "10 - DICE",
			};
			changed = ImGuiWidgets::EnumCombo(label, kNames, &newVal);
			hovered = ImGui::IsItemHovered();
		}
		else if (hasAnn && ann.Widget == EffectRecord::MenuWidget::Enum) {
			std::vector<std::string> labels;
			std::stringstream ss(ann.EnumNames);
			std::string item;
			while (std::getline(ss, item, ',')) labels.push_back(item);
			if (!labels.empty())
				changed = ImGuiWidgets::EnumCombo(label, labels, &newVal);
			else // malformed/empty enumNames: degrade to the int type-default rather than crash
				changed = ImGuiWidgets::IntDrag(label, &newVal, (int)ann.Min, (int)ann.Max, (int)ann.Step, false);
			hovered = ImGui::IsItemHovered();
		}
		else if (hasAnn && ann.Widget == EffectRecord::MenuWidget::Key) {
			changed = ImGuiWidgets::KeyBindPicker(label, &newVal, &hovered);
		}
		else if (hasAnn) {
			bool useSlider = (ann.Widget == EffectRecord::MenuWidget::Slider);
			changed = ImGuiWidgets::IntDrag(label, &newVal, (int)ann.Min, (int)ann.Max, (int)ann.Step, useSlider);
			hovered = ImGui::IsItemHovered();
		}
		else {
			changed = ImGuiWidgets::IntDrag(label, &newVal, 0, 0, 1, false); // unbounded, matches pre-R3 behavior
			hovered = ImGui::IsItemHovered();
		}

		if (changed && newVal != val) {
			TheSettingManager->SetSetting(node.Section, node.Key, newVal);
			TheSettingManager->LoadSettings();
		}
		syncLiveConstant((float)newVal);
		ImGuiWidgets::TooltipIfHovered(hovered, tooltip);
		if (ImGuiWidgets::RevertButton(isDirty)) revert();
		break;
	}
	default: { // String
		static std::unordered_map<ImGuiID, std::string> sBufs;
		ImGuiID id = ImGui::GetID(node.Key);
		std::string& persistent = sBufs[id];
		if (ImGui::GetActiveID() != id)
			persistent = node.Value;
		char buf[80];
		strncpy_s(buf, persistent.c_str(), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		if (ImGui::InputText(label, buf, sizeof(buf)))
			persistent = buf;
		bool hovered = ImGui::IsItemHovered();
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			TheSettingManager->SetSettingS(node.Section, node.Key, buf);
			TheSettingManager->LoadSettings();
			// `path` widget uniforms (e.g. LUTEffect's DayLUT/NightLUT/InteriorLUT,
			// once ported -- see docs/refactor-plan.md) get their side effect here.
			// A real prev/next FilePicker for annotated path uniforms is future
			// work (R3f); this commits whatever text was typed either way.
			if (owner && hasAnn && ann.Widget == EffectRecord::MenuWidget::Path) {
				owner->OnPathChanged(uniformName.c_str(), buf);
			}
		}
		ImGuiWidgets::TooltipIfHovered(hovered, tooltip);
		if (ImGuiWidgets::RevertButton(isDirty)) revert();
		break;
	}
	}

	ImGui::PopID();
}

static void RenderMenuColorTriple(
	SettingManager::Configuration::ConfigNode& nodeR,
	SettingManager::Configuration::ConfigNode& nodeG,
	SettingManager::Configuration::ConfigNode& nodeB,
	const std::string& prefix,
	EffectRecord* owner)
{
	(void)owner; // float3 `color` annotations aren't representable by three scalar
	             // ConfigNodes yet -- this grouping is still detected by the R,G,B
	             // key-suffix convention alone; see the comment at its call site.

	if (s_menuColorStatesNeedReset) {
		s_menuColorStates.clear();
		s_menuColorStatesNeedReset = false;
	}

	std::string stateKey = std::string(nodeR.Section) + "." + prefix;
	ImGuiWidgets::ColorTripleState& state = s_menuColorStates[stateKey];

	float rgb[3] = { nodeR.FloatValue, nodeG.FloatValue, nodeB.FloatValue };

	// Trim trailing underscore for display (matches pre-R3 behavior).
	std::string label = prefix;
	while (!label.empty() && label.back() == '_') label.pop_back();

	auto snapOf = [&](SettingManager::Configuration::ConfigNode& n) -> std::string& {
		auto& sec = s_menuSnapshot[n.Section];
		auto  it  = sec.find(n.Key);
		if (it == sec.end()) it = sec.emplace(n.Key, n.Value).first;
		return it->second;
	};
	std::string& snapR = snapOf(nodeR);
	std::string& snapG = snapOf(nodeG);
	std::string& snapB = snapOf(nodeB);
	bool isDirty = snapR != nodeR.Value || snapG != nodeG.Value || snapB != nodeB.Value;

	ImGui::PushID(prefix.c_str());
	ImGui::TextUnformatted(label.c_str());
	if (ImGuiWidgets::RevertButton(isDirty)) {
		TheSettingManager->SetSettingS(nodeR.Section, nodeR.Key, const_cast<char*>(snapR.c_str()));
		TheSettingManager->SetSettingS(nodeG.Section, nodeG.Key, const_cast<char*>(snapG.c_str()));
		TheSettingManager->SetSettingS(nodeB.Section, nodeB.Key, const_cast<char*>(snapB.c_str()));
		TheSettingManager->LoadSettings();
		state.seeded = false; // reseed from the just-reverted values next frame
	}

	bool hovered = false;
	bool changed = ImGuiWidgets::ColorTriplePicker("##triple", rgb, &state, &hovered);
	if (changed) {
		TheSettingManager->SetSetting(nodeR.Section, nodeR.Key, rgb[0]);
		TheSettingManager->SetSetting(nodeG.Section, nodeG.Key, rgb[1]);
		TheSettingManager->SetSetting(nodeB.Section, nodeB.Key, rgb[2]);
		TheSettingManager->LoadSettings();
	}

	// Unlike the pre-R3 implementation (which showed this tooltip
	// unconditionally whenever a description was present), TooltipIfHovered
	// correctly gates on hover -- see "Tooltip rendered in 3 branches (one
	// unreachable)" in docs/refactor-plan.md's "What This Eliminates" table.
	ImGuiWidgets::TooltipIfHovered(hovered, nodeR.Description.empty() ? nullptr : nodeR.Description.c_str());

	ImGui::PopID();
}

void EffectRecord::RenderGenericSection(const char* section, bool isShaderSection, EffectRecord* owner) {
	SettingManager::Configuration::SettingList settings;
	TheSettingManager->FillMenuSettings(&settings, section);

	if (settings.empty()) {
		ImGui::TextDisabled("No settings in this section.");
		return;
	}

	// LUTEffect's day/night/interior cycle pickers are chrome that predates R3
	// and hasn't been ported to the `path` widget + OnPathChanged() hook
	// described in docs/refactor-plan.md yet (own uniforms are still hidden
	// via ShouldHideMenuKey() below and rendered specially here) -- left
	// unchanged aside from routing through the new FilePicker primitive.
	if (strcmp(section, "Shaders.LUT.Main") == 0) {
		LUTEffect* lut = TheShaderManager->Effects.LUT;
		if (lut) {
			if (lut->Settings.PreTonemapping) {
				if (lut->Settings.HDRCompat)
					ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "HDR Compat: SDR LUTs normalized to HDR range (approximate).");
				else
					ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Pre-tonemapping mode: use HDR LUTs (float16 DDS converted from .CUBE).");
				ImGui::Spacing();
			}
			if (lut->LUTFiles.empty()) {
				ImGui::TextDisabled("No LUTs found in Data/Textures/NewVegasReloaded/LUTs/");
			} else {
				auto renderPicker = [&](const char* label, int& idx, int slot) {
					if (ImGuiWidgets::FilePicker(label, lut->LUTFiles, &idx))
						lut->LoadLUT(slot, lut->LUTFiles[idx].c_str());
				};
				renderPicker("Day     ", lut->DayIdx,      0);
				renderPicker("Night   ", lut->NightIdx,    1);
				renderPicker("Interior", lut->InteriorIdx, 2);
			}
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
		}
	}

	std::unordered_map<std::string, int> keyIdx;
	for (int i = 0; i < (int)settings.size(); i++)
		keyIdx[settings[i].Key] = i;

	std::unordered_set<std::string> handled;

	for (auto& s : settings) {
		std::string key(s.Key);
		if (handled.count(key)) continue;
		if (ShouldHideMenuKey(s.Key)) continue;

		// Hide HDRCompat when PreTonemapping is off -- meaningless in post-tonemapping mode.
		if (strcmp(s.Key, "HDRCompat") == 0 && strcmp(section, "Shaders.LUT.Main") == 0) {
			LUTEffect* lut = TheShaderManager->Effects.LUT;
			if (lut && !lut->Settings.PreTonemapping) continue;
		}

		// RGB triple -> colour picker (shader sections only). Still detected
		// by the R/G/B key-suffix convention rather than a `color` annotation:
		// no shipped uniform is both float3-typed and annotated yet (packed
		// constants like TESR_ColoringColorCurve don't correspond 1:1 with
		// these per-channel settings), so there's nothing for a per-uniform
		// lookup to match against today. See docs/refactor-plan.md R3e/R3f.
		if (isShaderSection && key.size() > 1 && key.back() == 'R') {
			std::string pfx = key.substr(0, key.size() - 1);
			std::string kG = pfx + "G", kB = pfx + "B";
			bool isColorCurve = (pfx == "ColorCurve" && strstr(s.Section, "Shaders.Coloring") != nullptr);
			if (!isColorCurve && keyIdx.count(kG) && keyIdx.count(kB)) {
				handled.insert(key);
				handled.insert(kG);
				handled.insert(kB);
				RenderMenuColorTriple(s, settings[keyIdx[kG]], settings[keyIdx[kB]], pfx, owner);
				continue;
			}
		}

		RenderMenuNode(s, isShaderSection, owner);
	}
}

void EffectRecord::RenderMenu(const char* section) {
	RenderGenericSection(section, /*isShaderSection=*/true, this);
}

void EffectRecord::ResetMenuState() {
	s_menuSnapshot.clear();
	s_menuColorStatesNeedReset = true;
}

void EffectRecord::RevertMenuSnapshot() {
	for (auto& [section, keys] : s_menuSnapshot)
		for (auto& [key, value] : keys)
			TheSettingManager->SetSettingS(const_cast<char*>(section.c_str()),
			                               const_cast<char*>(key.c_str()),
			                               const_cast<char*>(value.c_str()));
	TheSettingManager->LoadSettings();
	ResetMenuState();
}

void EffectRecord::SnapshotSettingIfUnseen(const char* section, const char* key) {
	auto& sectionSnap = s_menuSnapshot[section];
	if (sectionSnap.find(key) != sectionSnap.end()) return; // already captured this session

	SettingManager::Configuration::ConfigNode node;
	if (TheSettingManager->Config.FillNode(&node, section, key))
		sectionSnap.emplace(key, node.Value);
}

/*
 * R3d: HLSL <> annotation parser. Reads widget/name/description/defaultValue/
 * min/max/step/folder/filter/enumNames off a TESR_-prefixed uniform's
 * annotation block via the effect's own constant table (ID3DXBaseEffect::
 * GetAnnotation()) -- no sidecar files, no separate schema, matching the
 * locked format in docs/refactor-plan.md R3a.
 *
 * R3f-1 extended this to also read componentKeys/componentNames/
 * componentDefaults/componentMins/componentMaxs/componentSteps for the
 * `packed` widget (docs/refactor-plan.md "Packed Components") -- a uniform
 * that backs N independent settings rather than one. Like enumNames, these
 * are stored raw (comma-delimited, x/y/z/w order); this parser does not
 * split or interpret them. Nothing calls GetUniformAnnotation() looking for
 * these fields yet -- that's R3f-2's per-effect uniform index -- so this is
 * additive plumbing only, no behavior change.
 *
 * Every D3DX call here is guarded: a missing uniform, a missing annotation
 * block, an annotation whose value can't be read in its declared type, or an
 * unrecognised `widget` string are all handled by falling back rather than
 * failing -- "malformed blocks must not crash" and "unknown hints fall back
 * silently to the type-default" are both hard requirements per R3a, not just
 * the happy path. `defaultValue` is the one required field: its absence (or an
 * unreadable value) means this uniform isn't menu-visible at all, and
 * GetUniformAnnotation() returns false with `out` untouched -- there is no
 * partially-populated result.
 */
bool EffectRecord::GetUniformAnnotation(const char* uniformName, UniformAnnotation* out) {
	if (!Effect || !uniformName || !out) return false;

	D3DXHANDLE param = Effect->GetParameterByName(NULL, uniformName);
	if (!param) return false; // no such uniform on this effect

	D3DXPARAMETER_DESC paramDesc;
	if (FAILED(Effect->GetParameterDesc(param, &paramDesc))) return false;
	if (paramDesc.Annotations == 0) return false; // uniform exists, but carries no <> block at all

	UniformAnnotation ann; // built up locally -- *out is only touched once `defaultValue` is confirmed present
	bool hasDefault = false, minSeen = false, maxSeen = false, stepSeen = false;

	for (UINT i = 0; i < paramDesc.Annotations; i++) {
		D3DXHANDLE annHandle = Effect->GetAnnotation(param, i);
		if (!annHandle) continue; // malformed entry -- skip it, don't crash

		D3DXPARAMETER_DESC annDesc;
		if (FAILED(Effect->GetParameterDesc(annHandle, &annDesc)) || !annDesc.Name) continue;

		const char* field = annDesc.Name;

		if (!_stricmp(field, "name")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.Name = s;
		}
		else if (!_stricmp(field, "description")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.Description = s;
		}
		else if (!_stricmp(field, "widget")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) {
				if      (!_stricmp(s, "color"))  ann.Widget = MenuWidget::Color;
				else if (!_stricmp(s, "enum"))   ann.Widget = MenuWidget::Enum;
				else if (!_stricmp(s, "key"))    ann.Widget = MenuWidget::Key;
				else if (!_stricmp(s, "slider")) ann.Widget = MenuWidget::Slider;
				else if (!_stricmp(s, "path"))   ann.Widget = MenuWidget::Path;
				else if (!_stricmp(s, "hidden")) ann.Widget = MenuWidget::Hidden;
				else if (!_stricmp(s, "packed")) ann.Widget = MenuWidget::Packed;
				else                              ann.Widget = MenuWidget::Default; // unrecognised hint -> type-default
			}
		}
		else if (!_stricmp(field, "min")) {
			float f;
			if (SUCCEEDED(Effect->GetFloat(annHandle, &f))) { ann.Min = f; minSeen = true; }
		}
		else if (!_stricmp(field, "max")) {
			float f;
			if (SUCCEEDED(Effect->GetFloat(annHandle, &f))) { ann.Max = f; maxSeen = true; }
		}
		else if (!_stricmp(field, "step")) {
			float f;
			if (SUCCEEDED(Effect->GetFloat(annHandle, &f))) { ann.Step = f; stepSeen = true; }
		}
		else if (!_stricmp(field, "folder")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.Folder = s;
		}
		else if (!_stricmp(field, "filter")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.Filter = s;
		}
		else if (!_stricmp(field, "enumNames")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.EnumNames = s;
		}
		// R3f-1: Packed widget fields (docs/refactor-plan.md "Packed Components").
		// Stored raw, same as EnumNames above -- splitting the comma-delimited
		// lists and matching them up against a settings section is the
		// consumer's job (R3f-2's per-effect uniform index), not this parser's.
		else if (!_stricmp(field, "componentKeys")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.ComponentKeys = s;
		}
		else if (!_stricmp(field, "componentNames")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.ComponentNames = s;
		}
		else if (!_stricmp(field, "componentDefaults")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.ComponentDefaults = s;
		}
		else if (!_stricmp(field, "componentMins")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.ComponentMins = s;
		}
		else if (!_stricmp(field, "componentMaxs")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.ComponentMaxs = s;
		}
		else if (!_stricmp(field, "componentSteps")) {
			LPCSTR s = nullptr;
			if (SUCCEEDED(Effect->GetString(annHandle, &s)) && s) ann.ComponentSteps = s;
		}
		else if (!_stricmp(field, "defaultValue")) {
			// The only required field -- named defaultValue, not default:
			// `default` is a reserved HLSL keyword and the D3DX9 effect
			// compiler rejects it as an annotation field name outright
			// (error X3000: syntax error: unexpected token 'default'),
			// confirmed against a real build. See docs/refactor-plan.md's
			// "Correction" note under Shader Annotation Format.
			// min/max/step, GetFloat/GetInt/GetBool/
			// GetString all vary by the uniform's own declared type (float,
			// float2-4, int, bool, string) -- rather than duplicate that
			// type dispatch here for a value nothing downstream reads yet
			// (persisted values come from SettingManager, not the shader;
			// annotation-default-as-initial-value is R4's job once a setting
			// can be discovered with no pre-existing TOML entry), this just
			// confirms the field is present and takes its type from the
			// annotation's own successful GetParameterDesc() above.
			hasDefault = true;
		}
		// Unrecognised annotation names are ignored -- forward compatible
		// with fields added later, never an error.
	}

	if (!hasDefault) return false; // required field missing: not menu-visible

	// Un-annotated min/max/step fall back to the type-default table in
	// docs/refactor-plan.md, which differs for float vs. int uniforms.
	bool isIntType = (paramDesc.Type == D3DXPT_INT);
	if (!minSeen)  ann.Min  = 0.0f;
	if (!maxSeen)  ann.Max  = isIntType ? 10.0f : 1.0f;
	if (!stepSeen) ann.Step = isIntType ? 1.0f : 0.001f;

	ann.HasDefault = true;
	*out = ann;
	return true;
}

/*
 * R3f-2: builds PackedSettings (docs/refactor-plan.md "Packed Components" /
 * "Consumption") by walking every TESR_-prefixed uniform on this effect's own
 * constant table -- same GetDesc()/GetParameter() loop CreateCT() already
 * does, but over annotations instead of GPU register classes -- and, for
 * each one whose annotation resolves to widget=packed, exploding its
 * componentKeys into individual PackedComponentRef entries. This is the
 * effect's own settings surface being read from the shader for once, rather
 * than the shader being consulted only after a "TESR_" + effect name + key
 * guess already found something -- the actual "annotation-driven" half of
 * R3f, scoped to just packed uniforms today.
 *
 * Called once whenever this effect (re)loads (see LoadEffect(), right after
 * CreateCT()) -- not per-frame, not per-menu-render. A uniform contributes
 * nothing to the index, rather than a crash or a partial entry, if its
 * annotation can't be read, isn't widget=packed, or has an empty/malformed
 * componentKeys.
 */
void EffectRecord::BuildPackedSettingsIndex() {
	PackedSettings.clear();
	if (!Effect) return;

	D3DXEFFECT_DESC effectDesc;
	if (FAILED(Effect->GetDesc(&effectDesc))) return;

	for (UINT c = 0; c < effectDesc.Parameters; c++) {
		D3DXHANDLE handle = Effect->GetParameter(NULL, c);
		D3DXPARAMETER_DESC paramDesc;
		if (FAILED(Effect->GetParameterDesc(handle, &paramDesc)) || !paramDesc.Name) continue;
		if (memcmp(paramDesc.Name, "TESR_", 5)) continue; // only TESR_-prefixed carry annotations we care about
		if (paramDesc.Annotations == 0) continue; // fast skip -- no <> block at all, can't be `packed`

		UniformAnnotation ann;
		if (!GetUniformAnnotation(paramDesc.Name, &ann)) continue; // no defaultValue -> not menu-visible at all
		if (ann.Widget != MenuWidget::Packed) continue;
		if (ann.ComponentKeys.empty()) continue; // malformed/empty -- nothing to index, degrade silently

		// Six comma-delimited lists, positionally aligned with each other
		// only -- not with this uniform's own x/y/z/w layout (see
		// docs/refactor-plan.md). Optional lists shorter than componentKeys,
		// or carrying an empty entry at a given position, fall back per-entry
		// the same way the singular name/min/max/step fields do.
		std::vector<std::string> keys  = SplitCsv(ann.ComponentKeys);
		std::vector<std::string> names = SplitCsv(ann.ComponentNames);
		std::vector<std::string> mins  = SplitCsv(ann.ComponentMins);
		std::vector<std::string> maxs  = SplitCsv(ann.ComponentMaxs);
		std::vector<std::string> steps = SplitCsv(ann.ComponentSteps);

		for (size_t i = 0; i < keys.size(); i++) {
			if (keys[i].empty()) continue; // stray comma or similar -- not a usable key, skip it

			PackedComponentRef ref;
			ref.UniformName = paramDesc.Name;
			ref.Name = (i < names.size() && !names[i].empty()) ? names[i] : keys[i];
			// No int-vs-float type-aware fallback here (unlike the scalar
			// case above) -- see docs/refactor-plan.md "Consumption" for why.
			ref.Min  = (i < mins.size()  && !mins[i].empty())  ? (float)atof(mins[i].c_str())  : 0.0f;
			ref.Max  = (i < maxs.size()  && !maxs[i].empty())  ? (float)atof(maxs[i].c_str())  : 1.0f;
			ref.Step = (i < steps.size() && !steps[i].empty()) ? (float)atof(steps[i].c_str()) : 0.001f;

			PackedSettings[keys[i]] = ref; // last writer wins if two packed uniforms claim the same key -- shouldn't happen, doesn't crash if it does
		}
	}
}
