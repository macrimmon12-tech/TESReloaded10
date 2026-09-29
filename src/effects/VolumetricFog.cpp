#include "VolumetricFog.h"
#include "../core/GpuProfiler.h"

void VolumetricFogEffect::UpdateConstants() {
	// live weather-driven sky-filter disable, smoothly animated the same way RainEffect
	// animates rain onset/offset (Animator started once on the true/false edge, sampled every frame).
	// Animator's clock runs in GAME HOURS, not real seconds (Animator.cpp derives currenttime from
	// GameDaysPassed) -- 0.1f here is a few game-minutes, a few real seconds at typical time scale,
	// matching the same order of magnitude RainEffect uses (0.05f/0.07f) rather than the ~6-real-minute
	// transition an unadjusted "3.0f meaning 3 seconds" mistake would have produced.
	bool shouldDisableFilter = (TheShaderManager->GameState.isRainy && rainyDisablesSkyFilter) ||
	                           (TheShaderManager->GameState.isCloudy && cloudyDisablesSkyFilter);

	if (shouldDisableFilter && weatherFilterWasActive) {
		// weather just started hiding the sky filter / disabling NVR-driven density
		weatherFilterWasActive = false;
		Constants.WeatherFilterAnimator.Start(0.1f, 0.0f);
	}
	else if (!shouldDisableFilter && !weatherFilterWasActive) {
		// weather cleared, sky filter/density fade back in
		weatherFilterWasActive = true;
		Constants.WeatherFilterAnimator.Start(0.1f, 1.0f);
	}

	Constants.Weather.x = Constants.WeatherFilterAnimator.GetValue();
	Constants.Weather.y = TheShaderManager->GameState.isExterior ? 1.0f : 0.0f;
	// SkyAmbientRadiance's TESR_SkyIrradiance is only refreshed while Shaders.Sky is enabled;
	// fall back to flat sky color in the shader when it's off rather than reading stale/zero data.
	Constants.Weather.z = TheShaderManager->Shaders.Sky->Enabled ? 1.0f : 0.0f;

	// Moon-phase-driven night ambient ceiling, mirroring ShadowsExterior.cpp's own moon-phase
	// shadow-fade formula (same day-count/phaseLength inputs, same cosine curve) so fog's night
	// floor tracks the same lunar cycle the shadow system already uses. The day/night fade itself
	// is left to the shader's own continuous isDayTime curve rather than duplicated here, so the
	// transition stays smooth instead of snapping at a hard cutoff. Harmless to compute for
	// interiors too -- NightAmbientStrength (read in UpdateSettings) is gated to zero there by the
	// shader's own nightFactor (isExterior term), so this has no effect regardless.
	TimeGlobals* GameTimeGlobals = TimeGlobals::Get();
	float DaysPassed = GameTimeGlobals->GameDaysPassed ? GameTimeGlobals->GameDaysPassed->data : 1.0f;
	float MoonPhase = (fmod(DaysPassed, 8 * Tes->sky->firstClimate->phaseLength & 0x3F)) / (Tes->sky->firstClimate->phaseLength & 0x3F);
	MoonPhase = std::lerp(-D3DX_PI, D3DX_PI, MoonPhase / 8) - D3DX_PI / 4;
	Constants.Global.z = std::lerp(0.0f, nightMinDarkness, cosf(MoonPhase) * 0.5f + 0.5f);
}

void VolumetricFogEffect::UpdateSettings(){

	char SettingCategory[50] = "Shaders.VolumetricFog.";

	if (TheShaderManager->GameState.isExterior)
		strcat(SettingCategory, "Main");
	else
		strcat(SettingCategory, "Interiors");

	Constants.Global.x = TheSettingManager->GetSettingF(SettingCategory, "Amount");
	Constants.Weather.w = TheSettingManager->GetSettingF(SettingCategory, "FogSaturation");

	Constants.Density.x = TheSettingManager->GetSettingF(SettingCategory, "BaseDensity");
	Constants.Density.y = TheSettingManager->GetSettingF(SettingCategory, "WeatherImpact");

	Constants.Shape.x = TheSettingManager->GetSettingF(SettingCategory, "HeightFalloff");
	Constants.Shape.y = TheSettingManager->GetSettingF(SettingCategory, "MaxHeight");
	Constants.Shape.z = TheSettingManager->GetSettingF(SettingCategory, "Extinction");
	Constants.Shape.w = TheSettingManager->GetSettingF(SettingCategory, "Inscattering");

	float windAngleRad = TheSettingManager->GetSettingF(SettingCategory, "WindAngle") * 0.0174532925f; // degrees to radians
	Constants.Wind.x = cosf(windAngleRad);
	Constants.Wind.y = sinf(windAngleRad);
	Constants.Wind.z = TheSettingManager->GetSettingF(SettingCategory, "WindSpeed");
	Constants.Wind.w = TheSettingManager->GetSettingF(SettingCategory, "NoiseScale");

	Constants.Scatter.z = TheSettingManager->GetSettingF(SettingCategory, "NoiseStrength");
	Constants.Scatter.w = TheSettingManager->GetSettingF(SettingCategory, "HeightInfluence");

	rainyDisablesSkyFilter = TheSettingManager->GetSettingI(SettingCategory, "RainyDisablesSkyFilter") != 0;
	cloudyDisablesSkyFilter = TheSettingManager->GetSettingI(SettingCategory, "CloudyDisablesSkyFilter") != 0;

	if (TheShaderManager->GameState.isExterior) {
		Constants.Density.z = TheSettingManager->GetSettingF(SettingCategory, "MorningFogDip");
		Constants.Density.w = TheSettingManager->GetSettingF(SettingCategory, "SunriseSunsetBoost");

		Constants.Scatter.x = TheSettingManager->GetSettingF(SettingCategory, "PhaseAsymmetry");
		Constants.Scatter.y = TheSettingManager->GetSettingF(SettingCategory, "ShadowStrength");

		Constants.Aerial.x = TheSettingManager->GetSettingF(SettingCategory, "AerialStrength");
		Constants.Aerial.y = TheSettingManager->GetSettingF(SettingCategory, "AerialRangeStart");
		Constants.Aerial.z = TheSettingManager->GetSettingF(SettingCategory, "AerialTintBlend");
		Constants.Aerial.w = TheSettingManager->GetSettingF(SettingCategory, "AerialDayFadeStart");

		Constants.AerialTintColor.x = TheSettingManager->GetSettingF(SettingCategory, "AerialTintR");
		Constants.AerialTintColor.y = TheSettingManager->GetSettingF(SettingCategory, "AerialTintG");
		Constants.AerialTintColor.z = TheSettingManager->GetSettingF(SettingCategory, "AerialTintB");
		Constants.AerialTintColor.w = TheSettingManager->GetSettingF(SettingCategory, "SunScatteringStrength");

		Constants.Distant.x = TheSettingManager->GetSettingF(SettingCategory, "DistantFogRange");
		Constants.Distant.y = TheSettingManager->GetSettingF(SettingCategory, "DistantFogBlend");
		Constants.Distant.z = TheSettingManager->GetSettingF(SettingCategory, "DistantFogHeight");
		Constants.Distant.w = TheSettingManager->GetSettingF(SettingCategory, "EdgeAA");

		// shared with ShadowsExterior's own moon-phase shadow fade -- keeps fog's night-ambient
		// ceiling consistent with the shadow system's, rather than a second, disconnected knob.
		nightMinDarkness = 1.0f - TheSettingManager->GetSettingF("Shaders.ShadowsExteriors.Main", "NightMinDarkness");

		Constants.Global.w = TheSettingManager->GetSettingF(SettingCategory, "MinDensityFloor");
	}
	else {
		// these settings don't do anything in interiors: no sun, no horizon, no distant view
		Constants.Density.z = 0.0f;
		Constants.Density.w = 0.0f;

		Constants.Scatter.x = 0.0f;
		Constants.Scatter.y = 0.0f;

		Constants.Aerial = D3DXVECTOR4(0, 0, 0, 0);
		Constants.AerialTintColor = D3DXVECTOR4(0, 0, 0, 0);
		Constants.Distant = D3DXVECTOR4(0, 0, 0, 0);

		Constants.Global.w = 0.0f;
	}

	// Own section (not Main/Interiors-switched via SettingCategory), so it gets its own settings-UI
	// tab instead of crowding Main. Harmless to read unconditionally for interiors too -- the shader
	// gates its actual effect on isExterior the same way it already does for MorningFogDip's
	// timeOfDayScale, so it has no effect there regardless of what's read here.
	Constants.Night.x = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "DensityScale");
	Constants.Night.y = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "AmountScale");
	Constants.Night.z = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "HeightFalloffScale");
	Constants.Night.w = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "MaxHeightOffset");

	Constants.NightScatter.x = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "NoiseStrengthScale");
	Constants.NightScatter.y = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "WindSpeedScale");
	Constants.NightScatter.z = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "ExtinctionScale");
	Constants.NightScatter.w = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "InscatteringScale");

	Constants.NightTint.x = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "TintR");
	Constants.NightTint.y = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "TintG");
	Constants.NightTint.z = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "TintB");
	// Moved here from Global.y (Main-scoped) so it fades via the shared shader-side nightFactor like
	// the rest of this section, instead of needing its own interior-zeroing branch above.
	Constants.NightTint.w = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", "NightAmbientStrength");
	// Packed into Global.y (its spare slot) rather than a new vector for one bool -- see the shader's
	// skyMaskFactor for how it's used.
	Constants.Global.y = TheSettingManager->GetSettingI("Shaders.VolumetricFog.Night", "DisableSkyMask") ? 1.0f : 0.0f;
}

void VolumetricFogEffect::RegisterConstants(){
	TheShaderManager->RegisterConstant("TESR_VolumetricFogDensity", &Constants.Density);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogShape", &Constants.Shape);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogWind", &Constants.Wind);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogScatter", &Constants.Scatter);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogWeather", &Constants.Weather);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogAerial", &Constants.Aerial);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogAerialTint", &Constants.AerialTintColor);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogDistant", &Constants.Distant);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogGlobal", &Constants.Global);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogNight", &Constants.Night);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogNightScatter", &Constants.NightScatter);
	TheShaderManager->RegisterConstant("TESR_VolumetricFogNightTint", &Constants.NightTint);
}


bool VolumetricFogEffect::ShouldRender()
{
	return !TheShaderManager->GameState.isUnderwater;
};

void VolumetricFogEffect::RegisterTextures() {
	const int width = (TheRenderManager->width + 1) / 2;
	const int height = (TheRenderManager->height + 1) / 2;
	TheTextureManager->InitTexture("NVR_FogBuffer0", &fogTexture[0], &fogSurface[0], width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("NVR_FogBuffer1", &fogTexture[1], &fogSurface[1], width, height, D3DFMT_A16B16G16R16F);
}

/*
* Half-resolution estimate of the fog coefficients (multiply + depth, add) into two dedicated
* targets in a single MRT draw, then a depth-aware full-resolution reconstruct that applies them
* to the full-resolution scene. The scene is read from RenderedSurface, which every preceding
* effect leaves equal to the render target. Returns false without touching the render target
* if the dedicated path is unavailable.
*/
/*
* Whether this frame's fog can take over the shadow/AO apply passes: the dedicated path must be
* usable, and the composite AO upsample must know the AO targets' size -- from NVR_CompositeAOTexel,
* or, in an older effect file without it, by the AO targets matching the fog targets. Checked before
* those passes are skipped, so a later failure is exceptional.
*/
bool VolumetricFogEffect::CanComposite(IDirect3DSurface9* aoSurface) {
	if (!Enabled || !Effect || !ShouldRender() || dedicatedFogFailed || !fogSurface[0] ||
		TheSettingManager->SettingsMain.Main.DisableCompositeApply ||
		!Effect->GetTechniqueByName("CompositeFog") || !Effect->GetParameterByName(NULL, "NVR_CompositeFlags"))
		return false;
	if (aoSurface && !Effect->GetParameterByName(NULL, "NVR_CompositeAOTexel")) {
		D3DSURFACE_DESC fog = {}, ao = {};
		if (FAILED(fogSurface[0]->GetDesc(&fog)) || FAILED(aoSurface->GetDesc(&ao)) ||
			fog.Width != ao.Width || fog.Height != ao.Height)
			return false;
	}
	return true;
}

bool VolumetricFogEffect::RenderDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface) {
	if (dedicatedFogFailed) return false;
	const bool composite = compositeShadow || (compositeAO && compositeAOTexture);
	D3DXHANDLE technique = Effect->GetTechniqueByName(composite ? "CompositeFog" : "DedicatedFog");
	D3DXHANDLE flagsHandle = composite ? Effect->GetParameterByName(NULL, "NVR_CompositeFlags") : NULL;
	if (composite && !flagsHandle) return false;
	D3DXHANDLE layoutHandle = Effect->GetParameterByName(NULL, "NVR_FogLayout");
	D3DXTECHNIQUE_DESC description = {};
	D3DVIEWPORT9 original = {};
	D3DSURFACE_DESC target = {}, scratch = {}, fog = {}, fogAdd = {};
	D3DCAPS9 caps = {};
	IDirect3DTexture9* scene = TheTextureManager->RenderedTexture;
	if (!technique || !layoutHandle || FAILED(Effect->GetTechniqueDesc(technique, &description)) ||
		description.Passes != 2 || !RenderedSurface || !scene ||
		!fogTexture[0] || !fogTexture[1] || !fogSurface[0] || !fogSurface[1] ||
		FAILED(Device->GetDeviceCaps(&caps)) || caps.NumSimultaneousRTs < 2 ||
		FAILED(Device->GetViewport(&original)) || FAILED(RenderTarget->GetDesc(&target)) ||
		FAILED(RenderedSurface->GetDesc(&scratch)) || FAILED(fogSurface[0]->GetDesc(&fog)) ||
		FAILED(fogSurface[1]->GetDesc(&fogAdd)) ||
		original.X || original.Y ||
		original.Width != target.Width || original.Height != target.Height ||
		scratch.Width != target.Width || scratch.Height != target.Height ||
		fog.Width != (target.Width + 1) / 2 || fog.Height != (target.Height + 1) / 2 ||
		fogAdd.Width != fog.Width || fogAdd.Height != fog.Height || fogAdd.Format != fog.Format ||
		target.MultiSampleType != D3DMULTISAMPLE_NONE)
		return false;

	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = chain.Owns(RenderTarget, RenderedSurface);
	IDirect3DSurface9* finalTarget = chained ? chain.Output() : RenderTarget;
	D3DVIEWPORT9 reduced = original;
	reduced.Width = fog.Width;
	reduced.Height = fog.Height;
	D3DXVECTOR4 layout((float)reduced.Width / original.Width,
		(float)reduced.Height / original.Height, 1.0f / reduced.Width, 1.0f / reduced.Height);
	IDirect3DSurface9* depthSurface = nullptr;
	DWORD oldWriteMask1 = 0xF;
	HRESULT result = Device->GetRenderState(D3DRS_COLORWRITEENABLE1, &oldWriteMask1);
	if (SUCCEEDED(result)) result = Device->GetDepthStencilSurface(&depthSurface);
	if (SUCCEEDED(result)) result = Device->SetDepthStencilSurface(nullptr);
	if (FAILED(result)) {
		if (depthSurface) depthSurface->Release();
		dedicatedFogFailed = true;
		return false;
	}

	Effect->SetTechnique(technique);
	SetCT();
	Effect->SetVector(layoutHandle, &layout);
	if (composite) {
		D3DXVECTOR4 flags(compositeShadow ? 1.0f : 0.0f, (compositeAO && compositeAOTexture) ? 1.0f : 0.0f, 0.0f, 0.0f);
		Effect->SetVector(flagsHandle, &flags);
		// The AO targets are half or (AOLowRes) quarter resolution; tell the upsample which.
		D3DXHANDLE aoTexelHandle = Effect->GetParameterByName(NULL, "NVR_CompositeAOTexel");
		D3DSURFACE_DESC ao = {};
		if (aoTexelHandle && compositeAO && compositeAOTexture && SUCCEEDED(compositeAOTexture->GetLevelDesc(0, &ao))) {
			D3DXVECTOR4 aoTexel(1.0f / ao.Width, 1.0f / ao.Height, 0.0f, 0.0f);
			Effect->SetVector(aoTexelHandle, &aoTexel);
		}
	}
	UINT passes = 0;
	result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		static GpuTimer passTimers[2] = { GpuTimer("  Fog estimate (half)"), GpuTimer("  Fog composite (full)") };
		for (UINT p = 0; p < passes && SUCCEEDED(result); ++p) {
			const bool combine = p == passes - 1;
			GpuProfileScope gpuPass(passTimers[combine ? 1 : 0], Device);
			// s5 is TESR_PointShadowBuffer (bound by SetCT); the fog targets are s6/s7, AO s8.
			Device->SetTexture(6, nullptr);
			Device->SetTexture(7, nullptr);
			Device->SetTexture(8, nullptr);
			// Unbind the half-resolution MRT before binding the full-resolution target, so the
			// two differently sized targets are never bound together.
			if (combine) result = Device->SetRenderTarget(1, nullptr);
			if (SUCCEEDED(result)) result = Device->SetRenderTarget(0, combine ? finalTarget : fogSurface[0]);
			if (SUCCEEDED(result) && !combine) result = Device->SetRenderTarget(1, fogSurface[1]);
			if (SUCCEEDED(result) && !combine) result = Device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
			if (SUCCEEDED(result)) result = Device->SetViewport(combine ? &original : &reduced);
			if (FAILED(result)) break;
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			Device->SetTexture(0, scene); // TESR_SourceBuffer slot
			if (combine) {
				Device->SetTexture(6, fogTexture[0]);
				Device->SetTexture(7, fogTexture[1]);
				if (composite && compositeAO && compositeAOTexture) Device->SetTexture(8, compositeAOTexture);
			}
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
		}
		Effect->End();
	}
	Device->SetTexture(6, nullptr);
	Device->SetTexture(7, nullptr);
	Device->SetTexture(8, nullptr);
	Device->SetTexture(0, TheTextureManager->SourceTexture);
	Device->SetRenderTarget(1, nullptr);
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE1, oldWriteMask1);
	Device->SetViewport(&original);
	Device->SetDepthStencilSurface(depthSurface);
	if (depthSurface) depthSurface->Release();
	if (SUCCEEDED(result)) {
		if (chained) chain.Commit();
		else result = Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	}
	if (FAILED(result)) {
		// RenderedSurface is only written by the final copy, so it still holds the input.
		Device->StretchRect(RenderedSurface, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		dedicatedFogFailed = true;
		Logger::Log("Dedicated volumetric fog failed (%08lx); using packed path until restart.", result);
		return false;
	}
	compositeApplied = composite;
	static bool reportedComposite = false;
	if (composite && !reportedComposite) {
		Logger::Log("Composite apply active: sun shadows %s, AO %s folded into the fog pass.",
			compositeShadow ? "yes" : "no", (compositeAO && compositeAOTexture) ? "yes" : "no");
		reportedComposite = true;
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("Dedicated volumetric fog active: %ux%u -> %ux%u, full-resolution apply (MRT).",
			original.Width, original.Height, reduced.Width, reduced.Height);
		reported = true;
	}
	return true;
}

void VolumetricFogEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
	IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { renderTime = 0; return; }

	{
		auto timer = TimeLogger();
		if (RenderDedicated(Device, RenderTarget, RenderedSurface)) {
			renderTime = timer.LogTime("VolumetricFog::DedicatedRender");
			return;
		}
	}

	// The caller must apply deferred shadows/AO before retrying ordinary fog.
	if (compositeShadow || compositeAO) return;

	D3DXHANDLE technique = Effect->GetTechniqueByName("PackedFog");
	D3DXTECHNIQUE_DESC description = {};
	D3DVIEWPORT9 original = {};
	D3DSURFACE_DESC target = {}, scratch = {};
	if (packedFogFailed || !technique || !RenderedSurface || !SourceBuffer ||
		FAILED(Effect->GetTechniqueDesc(technique, &description)) || description.Passes != 2 ||
		FAILED(Device->GetViewport(&original)) || FAILED(RenderTarget->GetDesc(&target)) ||
		FAILED(RenderedSurface->GetDesc(&scratch)) || original.X || original.Y ||
		original.Width != target.Width || original.Height != target.Height ||
		target.Width != scratch.Width || target.Height != scratch.Height ||
		(target.Width & 1) || (target.Height & 1)) {
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}

	auto timer = TimeLogger();
	TheShaderManager->Chain.Sync(); // the packed path reads the render target itself
	D3DVIEWPORT9 reduced = original;
	reduced.Width /= 2;
	reduced.Height /= 2;
	RECT packed = {0, 0, (LONG)reduced.Width, (LONG)reduced.Height};
	HRESULT result = Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_NONE);
	if (SUCCEEDED(result)) result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) SetCT();
	UINT passes = 0;
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		for (UINT p = 0; p < passes && SUCCEEDED(result); ++p) {
			const bool combine = p == passes - 1;
			result = Device->SetViewport(combine ? &original : &reduced);
			if (FAILED(result)) break;
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
			if (SUCCEEDED(result)) result = Device->StretchRect(RenderTarget, combine ? NULL : &packed,
				RenderedSurface, combine ? NULL : &packed, D3DTEXF_NONE);
		}
		Effect->End();
	}
	Device->SetViewport(&original);

	if (FAILED(result)) {
		Device->StretchRect(SourceBuffer, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		Device->StretchRect(SourceBuffer, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
		packedFogFailed = true;
		Logger::Log("Packed volumetric fog failed (%08lx); using legacy path until restart.", result);
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("Packed volumetric fog active: %ux%u -> %ux%u.",
			original.Width, original.Height, reduced.Width, reduced.Height);
		reported = true;
	}
	renderTime = timer.LogTime("VolumetricFog::PackedRender");
}
