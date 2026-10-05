#include "GpuProfiler.h"
#include "PointShadowSlots.h"

#define RESZ_CODE 0x7FA05000

/**
* Initializes the Shader Manager Singleton.
* The Shader Manager creates and holds onto the Effects activated in the Settings Manager, and sets the constants.
*/
void ShaderManager::Initialize() {

	auto timer = TimeLogger();

	Logger::Log("Starting the shaders manager...");
	TheShaderManager = new ShaderManager();
	TheShaderManager->FrameVertex = NULL;

	TheShaderManager->EffectReloadQueued = false;

	memset(TheShaderManager->WaterVertexShaders, NULL, sizeof(WaterVertexShaders));
	memset(TheShaderManager->WaterPixelShaders, NULL, sizeof(WaterPixelShaders));

	TheShaderManager->CreateFrameVertex(TheRenderManager->width, TheRenderManager->height, &TheShaderManager->FrameVertex);

	TheShaderManager->PreviousCell = nullptr;
	TheShaderManager->IsMenuSwitch = false;

	// Make sure the shader/effect cache directories exist.
	std::error_code ec;

	std::filesystem::create_directories("data/Shaders/NewVegasReloaded/Shaders/Cache/Bink", ec);
	if (ec) {
		Logger::Log("Failed to create shader cache directory: %s", ec.message());
	}
	std::filesystem::create_directories("data/Shaders/NewVegasReloaded/Shaders/Cache/Shadows", ec);
	if (ec) {
		Logger::Log("Failed to create shader cache directory: %s", ec.message());
	}
	std::filesystem::create_directories("data/Shaders/NewVegasReloaded/Effects/Cache", ec);
	if (ec) {
		Logger::Log("Failed to create effect cache directory: %s", ec.message());
	}
	std::filesystem::create_directories("Data/Textures/NewVegasReloaded/LUTs", ec);
	if (ec) {
		Logger::Log("Failed to create LUT directory: %s", ec.message());
	}

	// initializing the list of effect names
	TheShaderManager->RegisterEffect<AvgLumaEffect>(&TheShaderManager->Effects.AvgLuma);
	TheShaderManager->RegisterEffect<AmbientOcclusionEffect>(&TheShaderManager->Effects.AmbientOcclusion);
	TheShaderManager->RegisterEffect<BloodLensEffect>(&TheShaderManager->Effects.BloodLens);
	TheShaderManager->RegisterEffect<BloomEffect>(&TheShaderManager->Effects.Bloom);
	TheShaderManager->RegisterEffect<BloomLegacyEffect>(&TheShaderManager->Effects.BloomLegacy);
	TheShaderManager->RegisterEffect<ColoringEffect>(&TheShaderManager->Effects.Coloring);
	TheShaderManager->RegisterEffect<LUTEffect>(&TheShaderManager->Effects.LUT);
	TheShaderManager->RegisterEffect<CinemaEffect>(&TheShaderManager->Effects.Cinema);
	TheShaderManager->RegisterEffect<CombineDepthEffect>(&TheShaderManager->Effects.CombineDepth);
	TheShaderManager->RegisterEffect<DepthOfFieldEffect>(&TheShaderManager->Effects.DepthOfField);
	TheShaderManager->RegisterEffect<DebugEffect>(&TheShaderManager->Effects.Debug);
	TheShaderManager->RegisterEffect<ExposureEffect>(&TheShaderManager->Effects.Exposure);
	TheShaderManager->RegisterEffect<FlashlightEffect>(&TheShaderManager->Effects.Flashlight);
	TheShaderManager->RegisterEffect<FlashlightBeamEffect>(&TheShaderManager->Effects.FlashlightBeam);
	TheShaderManager->RegisterEffect<GodRaysEffect>(&TheShaderManager->Effects.GodRays);
	TheShaderManager->RegisterEffect<ImageAdjustEffect>(&TheShaderManager->Effects.ImageAdjust);
	TheShaderManager->RegisterEffect<LensEffect>(&TheShaderManager->Effects.Lens);
	TheShaderManager->RegisterEffect<LowHFEffect>(&TheShaderManager->Effects.LowHF);
	TheShaderManager->RegisterEffect<MotionBlurEffect>(&TheShaderManager->Effects.MotionBlur);
	TheShaderManager->RegisterEffect<NormalsEffect>(&TheShaderManager->Effects.Normals);
	TheShaderManager->RegisterEffect<RainEffect>(&TheShaderManager->Effects.Rain);
	TheShaderManager->RegisterEffect<SharpeningEffect>(&TheShaderManager->Effects.Sharpening);
	TheShaderManager->RegisterEffect<ShadowsExteriorEffect>(&TheShaderManager->Effects.ShadowsExteriors);
	TheShaderManager->RegisterEffect<ShadowsInteriorsEffect>(&TheShaderManager->Effects.ShadowsInteriors);
	TheShaderManager->RegisterEffect<PointShadowsEffect>(&TheShaderManager->Effects.PointShadows);
	TheShaderManager->RegisterEffect<PointShadows2Effect>(&TheShaderManager->Effects.PointShadows2);
	TheShaderManager->RegisterEffect<SunShadowsEffect>(&TheShaderManager->Effects.SunShadows);
	TheShaderManager->RegisterEffect<SpecularEffect>(&TheShaderManager->Effects.Specular);
	TheShaderManager->RegisterEffect<SnowEffect>(&TheShaderManager->Effects.Snow);
	TheShaderManager->RegisterEffect<SnowAccumulationEffect>(&TheShaderManager->Effects.SnowAccumulation);
	TheShaderManager->RegisterEffect<UnderwaterEffect>(&TheShaderManager->Effects.Underwater);
	TheShaderManager->RegisterEffect<VolumetricLightEffect>(&TheShaderManager->Effects.VolumetricLight);
	TheShaderManager->RegisterEffect<VolumetricFogEffect>(&TheShaderManager->Effects.VolumetricFog);
	TheShaderManager->RegisterEffect<WaterLensEffect>(&TheShaderManager->Effects.WaterLens);
	TheShaderManager->RegisterEffect<WetWorldEffect>(&TheShaderManager->Effects.WetWorld);
	TheShaderManager->RegisterEffect<DitherBusterEffect>(&TheShaderManager->Effects.DitherBuster);
	TheShaderManager->RegisterEffect<SMAAEffect>(&TheShaderManager->Effects.SMAA);
	TheShaderManager->RegisterEffect<FXAAEffect>(&TheShaderManager->Effects.FXAA);
	TheShaderManager->RegisterEffect<TAAEffect>(&TheShaderManager->Effects.TAA);

	TheShaderManager->RegisterShaderCollection<TonemappingShaders>(&TheShaderManager->Shaders.Tonemapping);
	TheShaderManager->RegisterShaderCollection<POMShaders>(&TheShaderManager->Shaders.POM);
	TheShaderManager->RegisterShaderCollection<PBRShaders>(&TheShaderManager->Shaders.PBR);
	TheShaderManager->RegisterShaderCollection<WaterShaders>(&TheShaderManager->Shaders.Water);
	TheShaderManager->RegisterShaderCollection<SkyShaders>(&TheShaderManager->Shaders.Sky);
	TheShaderManager->RegisterShaderCollection<SkinShaders>(&TheShaderManager->Shaders.Skin);
	TheShaderManager->RegisterShaderCollection<GrassShaders>(&TheShaderManager->Shaders.Grass);
	TheShaderManager->RegisterShaderCollection<TerrainShaders>(&TheShaderManager->Shaders.Terrain);
	
	//setup map of constant names
	TheShaderManager->RegisterConstant("TESR_WorldTransform", (D3DXVECTOR4*)&TheRenderManager->worldMatrix);
	TheShaderManager->RegisterConstant("TESR_ViewTransform", (D3DXVECTOR4*)&TheRenderManager->viewMatrix);
	TheShaderManager->RegisterConstant("TESR_InvViewTransform", (D3DXVECTOR4*)&TheRenderManager->InvViewMatrix);
	TheShaderManager->RegisterConstant("TESR_ProjectionTransform", (D3DXVECTOR4*)&TheRenderManager->projMatrix);
	TheShaderManager->RegisterConstant("TESR_InvProjectionTransform",  (D3DXVECTOR4*)&TheRenderManager->InvProjMatrix);
	TheShaderManager->RegisterConstant("TESR_WorldViewProjectionTransform",  (D3DXVECTOR4*)&TheRenderManager->WorldViewProjMatrix);
	TheShaderManager->RegisterConstant("TESR_InvViewProjectionTransform", (D3DXVECTOR4*)&TheRenderManager->InvViewProjMatrix);
	TheShaderManager->RegisterConstant("TESR_ViewProjectionTransform", (D3DXVECTOR4*)&TheRenderManager->ViewProjMatrix);
	TheShaderManager->RegisterConstant("TESR_OcclusionWorldViewProjTransform", (D3DXVECTOR4*)&TheShaderManager->ShaderConst.OcclusionMap.OcclusionWorldViewProj);
	TheShaderManager->RegisterConstant("TESR_LightPosition", (D3DXVECTOR4*) &TheShaderManager->LightPosition);
	TheShaderManager->RegisterConstant("TESR_LightColor", (D3DXVECTOR4*) &TheShaderManager->LightColor);
	TheShaderManager->RegisterConstant("TESR_SpotLightPosition", (D3DXVECTOR4*) &TheShaderManager->SpotLightPosition);
	TheShaderManager->RegisterConstant("TESR_SpotLightColor", (D3DXVECTOR4*) &TheShaderManager->SpotLightColor);
	TheShaderManager->RegisterConstant("TESR_SpotLightDirection", (D3DXVECTOR4*) &TheShaderManager->SpotLightDirection);
	TheShaderManager->RegisterConstant("TESR_SpotLightToWorldTransform", (D3DXVECTOR4*) &TheShaderManager->SpotLightWorldToLightMatrix[0]);
	TheShaderManager->RegisterConstant("TESR_VolumetricData", &TheShaderManager->VolumetricData);
	TheShaderManager->RegisterConstant("TESR_ViewSpaceLightDir", &TheShaderManager->ShaderConst.ViewSpaceLightDir);
	TheShaderManager->RegisterConstant("TESR_ScreenSpaceLightDir", &TheShaderManager->ShaderConst.ScreenSpaceLightDir);
	TheShaderManager->RegisterConstant("TESR_ReciprocalResolution", &TheShaderManager->ShaderConst.ReciprocalResolution);
	TheShaderManager->RegisterConstant("TESR_CameraForward", &TheRenderManager->CameraForward);
	TheShaderManager->RegisterConstant("TESR_DepthConstants", &TheRenderManager->DepthConstants);
	TheShaderManager->RegisterConstant("TESR_CameraData", &TheRenderManager->CameraData);
	TheShaderManager->RegisterConstant("TESR_CameraPosition", &TheRenderManager->CameraPosition);
	TheShaderManager->RegisterConstant("TESR_SunDirection", &TheShaderManager->ShaderConst.SunDir);
	TheShaderManager->RegisterConstant("TESR_SunPosition", &TheShaderManager->ShaderConst.SunPosition);
	TheShaderManager->RegisterConstant("TESR_SunTiming", &TheShaderManager->ShaderConst.SunTiming);
	TheShaderManager->RegisterConstant("TESR_SunAmount", &TheShaderManager->ShaderConst.SunAmount);
	TheShaderManager->RegisterConstant("TESR_GameTime", &TheShaderManager->ShaderConst.GameTime);
	TheShaderManager->RegisterConstant("TESR_FogData", &TheShaderManager->ShaderConst.fogData);
	TheShaderManager->RegisterConstant("TESR_FogDistance", &TheShaderManager->ShaderConst.fogDistance);
	TheShaderManager->RegisterConstant("TESR_FogColor", &TheShaderManager->ShaderConst.fogColor);
	TheShaderManager->RegisterConstant("TESR_SunColor", &TheShaderManager->ShaderConst.sunColor);
	TheShaderManager->RegisterConstant("TESR_SunDiskColor", &TheShaderManager->ShaderConst.sunDiskColor);
	TheShaderManager->RegisterConstant("TESR_SunAmbient", &TheShaderManager->ShaderConst.sunAmbient);
	TheShaderManager->RegisterConstant("TESR_SkyColor", &TheShaderManager->ShaderConst.skyColor);
	TheShaderManager->RegisterConstant("TESR_SkyLowColor", &TheShaderManager->ShaderConst.skyLowColor);
	TheShaderManager->RegisterConstant("TESR_HorizonColor", &TheShaderManager->ShaderConst.horizonColor);

	TheShaderManager->InitializeConstants();

	timer.LogTime("ShaderManager::Initialize");
}

void ShaderManager::CreateFrameVertex(UInt32 Width, UInt32 Height, IDirect3DVertexBuffer9** FrameVertex) {
	
	void* VertexData = NULL;
	float OffsetX = (1.0f / (float)Width) * 0.5f;
	float OffsetY = (1.0f / (float)Height) * 0.5f;
	
	FrameVS FrameVertices[] = {
		{ -1.0f,  1.0f, 1.0f, 0.0f + OffsetX, 0.0f + OffsetY },
		{ -1.0f, -1.0f, 1.0f, 0.0f + OffsetX, 1.0f + OffsetY },
		{  1.0f,  1.0f, 1.0f, 1.0f + OffsetX, 0.0f + OffsetY },
		{  1.0f, -1.0f, 1.0f, 1.0f + OffsetX, 1.0f + OffsetY }
	};
	TheRenderManager->device->CreateVertexBuffer(4 * sizeof(FrameVS), D3DUSAGE_WRITEONLY, FrameFVF, D3DPOOL_DEFAULT, FrameVertex, NULL);
	(*FrameVertex)->Lock(0, 0, &VertexData, NULL);
	memcpy(VertexData, FrameVertices, sizeof(FrameVertices));
	(*FrameVertex)->Unlock();

}


/*
* Initializes and register an effect and its constants
*/
template <typename T> void ShaderManager::RegisterEffect(T** Pointer)
{
	T* effect = new T();
	*Pointer = effect;

	EffectsNames[effect->Name] = (EffectRecord**)Pointer;
	effect->UpdateSettings();
	effect->RegisterConstants();
	effect->RegisterTextures();
	effect->LoadEffect();
}


template <typename T> void ShaderManager::RegisterShaderCollection(T** Pointer)
{
	T* collection = new T();
	*Pointer = collection;
	
	ShaderNames[collection->Name] = (ShaderCollection**)Pointer;
	collection->RegisterConstants();
}


/*
 * Drops the cached texture pointer for a named sampler across EVERY loaded game shader.
 *
 * Counterpart to EffectRecord's per-effect ClearSampler. Call both whenever a TESR_ texture
 * is released and recreated (see ShadowsExteriorEffect::RecreateTextures): each shader owns
 * a private TextureRecord holding a raw IDirect3DTexture9*, which SetCT only re-resolves
 * when it is null. Miss one and it keeps sampling the released texture -- which the D3D9
 * device is usually still holding a reference to, so instead of failing it quietly returns
 * the last contents that were rendered into it.
 */
void ShaderManager::ClearShaderSamplers(const char* TextureName, size_t Length)
{
	// Effects as well as game shaders. This used to walk ShaderNames alone, so effects had to be
	// cleared individually by name at each call site -- and only SunShadows ever was. Any other
	// effect sampling a recreated texture kept its dangling pointer, which the device still
	// references, so it silently went on reading whatever was last rendered into the dead one.
	// VolumetricLight samples TESR_ShadowAtlas and hit exactly that: after any shadow setting
	// change its shafts were carved by a frozen copy of the atlas and no longer matched the
	// scene. Covering every effect here fixes it for all of them rather than adding one more
	// name to a list that has to be maintained by hand.
	for (const auto& Entry : EffectsNames) {
		EffectRecord* Effect = Entry.second ? *Entry.second : nullptr;
		if (Effect) Effect->ClearSampler(TextureName, Length);
	}

	for (const auto& Entry : ShaderNames) {
		ShaderCollection* Collection = Entry.second ? *Entry.second : nullptr;
		if (!Collection) continue;

		for (auto& VertexShader : Collection->VertexShaderList) {
			for (int i = 0; i < 3; i++)  // Default / Exterior / Interior
				if (VertexShader->ShaderProg[i]) VertexShader->ShaderProg[i]->ClearSampler(TextureName, Length);
		}
		for (auto& PixelShader : Collection->PixelShaderList) {
			for (int i = 0; i < 3; i++)
				if (PixelShader->ShaderProg[i]) PixelShader->ShaderProg[i]->ClearSampler(TextureName, Length);
		}
	}
}


void ShaderManager::RegisterConstant(const char* Name, D3DXVECTOR4* FloatValue)
{
	ConstantsTable[Name] = FloatValue;
}


void ShaderManager::InitializeConstants() {

	ShaderConst.pWeather = NULL;

	ShaderConst.ReciprocalResolution.x = 1.0f / (float)TheRenderManager->width;
	ShaderConst.ReciprocalResolution.y = 1.0f / (float)TheRenderManager->height;
	ShaderConst.ReciprocalResolution.z = (float)TheRenderManager->width / (float)TheRenderManager->height;
	ShaderConst.ReciprocalResolution.w = 0.0f; // Reserved to store the FoV
}


/*
Updates the values of the constants that can be accessed from shader code, with values representing the state of the game's elements.
*/
void ShaderManager::UpdateConstants() {

	if (!TheSettingManager->SettingsMain.Main.RenderEffects) return; // Main toggle

	auto timer = TimeLogger();

	bool IsThirdPersonView = !TheCameraManager->IsFirstPerson();
	Sky* WorldSky = Tes->sky;
	NiNode* SunRoot = WorldSky->sun->RootNode;
	TESClimate* currentClimate = WorldSky->firstClimate;
	TESWeather* currentWeather = WorldSky->firstWeather;
	TESWeather* previousWeather = WorldSky->secondWeather;
	TESObjectCELL* currentCell = Player->parentCell;
	TESWorldSpace* currentWorldSpace = Player->GetWorldSpace();
	TESRegion* currentRegion = Player->GetRegion();
	float weatherPercent = WorldSky->weatherPercent;
	float lastGameTime = ShaderConst.GameTime.y;
	const char* sectionName = NULL;
	avglumaRequired = false; // toggle for rendering AvgLuma
	orthoRequired = false; // toggle for rendering Ortho map

	if (Effects.Debug->Enabled) avglumaRequired = true;

	// context variables
	GameState.PipBoyIsOn = InterfaceManager->IsPipBoyOpen();
	GameState.VATSIsOn = InterfaceManager->IsActive(Menu::kMenuType_VATS);

	const bool bHasRTM = TheGameMenuManager->IsLiveMenu != nullptr;
	bool bComputersMenu = InterfaceManager->IsActive(Menu::kMenuType_Computers) && (bHasRTM ? (TheGameMenuManager->IsLiveMenu(Menu::kMenuType_Computers, false, false) == GameMenuManager::MENU_PAUSED) : true);
	if (!bComputersMenu)
		bComputersMenu = InterfaceManager->IsActive(Menu::kMenuType_Hacking) && (bHasRTM ? (TheGameMenuManager->IsLiveMenu(Menu::kMenuType_Hacking, false, false) == GameMenuManager::MENU_PAUSED) : true);
	bool bLockpickMenu = LockPickMenu::GetSingleton() && (bHasRTM ? (TheGameMenuManager->IsLiveMenu(Menu::kMenuType_LockPick, false, false) == GameMenuManager::MENU_PAUSED) : true);

	GameState.OverlayIsOn = bComputersMenu || bLockpickMenu ||
		InterfaceManager->IsActive(Menu::kMenuType_Surgery) ||
		InterfaceManager->IsActive(Menu::kMenuType_SlotMachine) ||
		InterfaceManager->IsActive(Menu::kMenuType_Blackjack) ||
		InterfaceManager->IsActive(Menu::kMenuType_Roulette) ||
		InterfaceManager->IsActive(Menu::kMenuType_Caravan);
	GameState.isDialog = InterfaceManager->IsActive(Menu::MenuType::kMenuType_Dialog);
	GameState.isPersuasion = InterfaceManager->IsActive(Menu::MenuType::kMenuType_Persuasion);
	
	if (!currentCell) return; // if no cell is present, we skip update to avoid trying to access values that don't exist
	
	GameState.isExterior = !currentCell->IsInterior();// || Player->parentCell->flags0 & TESObjectCELL::kFlags0_BehaveLikeExterior; // < use exterior flag, broken for now
	GameState.isCellChanged = currentCell != PreviousCell;
	PreviousCell = currentCell;
	if (GameState.isCellChanged) {
		TheSettingManager->SettingsChanged = true; // force update constants during cell transition
		// Preset resolve+apply as early in this function as possible -- before
		// anything below reads SettingsMain, so nothing this frame observes a
		// stale value (docs/preset-manager-design.md § "Application mechanism").
		//
		// Gated on the master toggle and on ShouldReResolve, not on isCellChanged
		// alone: Override presets are assigned per-worldspace outdoors (no
		// per-cell keyword tier exists for exteriors), so re-resolving on every
		// exterior cell border within the same worldspace was a no-op at best
		// and, at worst, silently clobbered any live tweak that hadn't been
		// saved yet the moment the player happened to cross one. ShouldReResolve
		// keeps interiors at today's per-cell granularity and always re-resolves
		// across an interior<->exterior boundary. The short-circuit order
		// matters: ShouldReResolve has side effects (updates its cached "last
		// resolved identity"), so it must not run while the master toggle is
		// off, or re-enabling it later would see a stale-but-matching cache and
		// skip the immediate resolve it needs to do.
		if (TheSettingManager->SettingsMain.Main.PresetManagerEnabled && PresetManager::ShouldReResolve(currentCell))
			PresetManager::ResolveAndApply(currentCell);
	}

	GameState.isUnderwater = Tes->sky->GetIsUnderWater();
	GameState.isRainy = currentWeather?currentWeather->GetWeatherType() == TESWeather::WeatherType::kType_Rainy : false;
	GameState.isSnow = currentWeather?currentWeather->GetWeatherType() == TESWeather::WeatherType::kType_Snow : false;
	GameState.isCloudy = currentWeather?currentWeather->GetWeatherType() == TESWeather::WeatherType::kType_Cloudy : false;

	TimeGlobals* GameTimeGlobals = TimeGlobals::Get();
	float GameHour = fmod(GameTimeGlobals->GameHour->data, 24); // make sure the hours values are less than 24

	float SunriseStart = WorldSky->GetSunriseBegin();
	float SunriseEnd = WorldSky->GetSunriseEnd();
	float SunsetStart = WorldSky->GetSunsetBegin();
	float SunsetEnd = WorldSky->GetSunsetEnd();

	// calculating sun amount for shaders (currently not used by any shaders)
	float sunRise = step(SunriseStart, SunriseEnd, GameHour); // 0 at night to 1 after sunrise
	float sunSet = step(SunsetEnd, SunsetStart, GameHour);  // 1 before sunset to 0 at night
	GameState.isDayTime = sunRise * sunSet;

	ShaderConst.SunTiming.x = WorldSky->GetSunriseColorBegin();
	ShaderConst.SunTiming.y = SunriseEnd;
	ShaderConst.SunTiming.z = SunsetStart;
	ShaderConst.SunTiming.w = WorldSky->GetSunsetColorEnd();

	// fake sunset time tracking with more time given before sunrise/sunset
	float sunRiseLight = step(SunriseStart - 1.0f, SunriseEnd - 1.0f, GameHour); // 0 at night to 1 after sunrise
	float sunSetLight = step(SunsetEnd + 1.0f, SunsetStart + 1.0f, GameHour);  // 1 before sunset to 0 at night
	float newDayLight = sunRiseLight * sunSetLight;
	float transitionPower = max(0.01f, TheSettingManager->SettingsMain.Transitions.TransitionCurvePower);
	GameState.transitionCurve = pow(smoothStep(0.0f, 1.0f, newDayLight), transitionPower); // a curve for day/night transitions that occurs mostly during second half of sunset

	GameState.isDayTimeChanged = (newDayLight != GameState.dayLight);  // allow effects to fire settings update during sunset/sunrise transitions
	GameState.dayLight = newDayLight;

	ShaderConst.GameTime.x = TimeGlobals::GetGameTime(); //time in milliseconds
	ShaderConst.GameTime.y = GameHour; //time in hours
	ShaderConst.GameTime.z = (float)TheFrameRateManager->Time;
	ShaderConst.GameTime.w = TheFrameRateManager->ElapsedTime; // frameTime in seconds

	ShaderConst.SunPosition = SunRoot->m_localTransform.pos.toD3DXVEC4();
	ShaderConst.SunPosition.w = 0.0f;
	// SunRoot (the sun disc's NiNode) is only positioned by the game's own sky-dome rendering,
	// which only runs while an exterior sky is actually drawn. On a fresh process load straight
	// into an interior save, before the player has ever seen an exterior sky this session, it
	// sits at its post-load default -- a zero vector -- and D3DXVec4Normalize of a zero-length
	// vector divides by zero, producing NaN. That NaN then poisons every downstream consumer of
	// SunPosition, notably EvalSky's per-sample radiance in SkyShaders::UpdateConstants (Sky.cpp)
	// -- every one of its 512 integration samples comes out NaN, so all 9 SH sky-irradiance
	// coefficients do too, and PBR Skylighting goes dark (indoors AND out) until the player
	// visits an exterior cell once and the sun disc gets a real position, matching the "only
	// works after having been outdoors" symptom exactly -- and matches SkyDebug's logged
	// Irradiance[0]=(nan,nan,nan) on a cold interior load.
	if (D3DXVec4LengthSq(&ShaderConst.SunPosition) > 0.0001f)
		D3DXVec4Normalize(&ShaderConst.SunPosition, &ShaderConst.SunPosition);
	else
		ShaderConst.SunPosition = D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f); // straight up: a safe, neutral default
	ShaderConst.SunPosition.w = 1.0f;

	ShaderConst.SunDir = Tes->directionalLight->direction.toD3DXVEC4() * -1.0f;
	ShaderConst.SunDir.w = 0.0f;
	D3DXVec4Normalize(&ShaderConst.SunDir, &ShaderConst.SunDir);
	ShaderConst.SunDir.w = 1.0f;

	// during the day, track the sun mesh position instead of the lighting direction in exteriors
	if (GameState.isExterior && GameState.dayLight > 0.5)
		ShaderConst.SunDir = ShaderConst.SunPosition;
	else
		ShaderConst.SunPosition.z = -ShaderConst.SunPosition.z;


	// expose the light vector in view space for screen space lighting
	D3DXVec4Transform(&ShaderConst.ScreenSpaceLightDir, &ShaderConst.SunDir, &TheRenderManager->ViewProjMatrix);
	D3DXVec4Normalize(&ShaderConst.ScreenSpaceLightDir, &ShaderConst.ScreenSpaceLightDir);

	D3DXVec4Transform(&ShaderConst.ViewSpaceLightDir, &ShaderConst.SunDir, &TheRenderManager->ViewMatrix);
	D3DXVec4Normalize(&ShaderConst.ViewSpaceLightDir, &ShaderConst.ViewSpaceLightDir);

	ShaderConst.sunGlare = currentWeather ? (currentWeather->GetSunGlare() / 255.0f) : 0.5f;

	ShaderConst.SunAmount.y = GameState.isDayTime; // accurate 0 - 1 value based on weather transition times
	GameState.isDayTime = smoothStep(0, 1, GameState.dayLight); // smooth daytime progression -- more accurate to light changes
	ShaderConst.SunAmount.x = GameState.isDayTime;

	ShaderConst.sunColor.x = WorldSky->sunDirectional.r;
	ShaderConst.sunColor.y = WorldSky->sunDirectional.g;
	ShaderConst.sunColor.z = WorldSky->sunDirectional.b;
	ShaderConst.sunColor.w = ShaderConst.sunGlare;

	if (Shaders.Sky->useSunDiskColor) {
		// experimental color used for more sky tinting capabilities
		ShaderConst.sunDiskColor.x = WorldSky->SunColor.r;
		ShaderConst.sunDiskColor.y = WorldSky->SunColor.g;
		ShaderConst.sunDiskColor.z = WorldSky->SunColor.b;
		ShaderConst.sunDiskColor.w = 1.0;
	}
	else {
		ShaderConst.sunDiskColor = ShaderConst.sunColor; // override with the color of the lighting
	}

	ShaderConst.windSpeed = WorldSky->windSpeed;

	ShaderConst.fogColor.x = WorldSky->fogColor.r;
	ShaderConst.fogColor.y = WorldSky->fogColor.g;
	ShaderConst.fogColor.z = WorldSky->fogColor.b;
	ShaderConst.fogColor.w = 1.0f;

	ShaderConst.horizonColor.x = WorldSky->Horizon.r;
	ShaderConst.horizonColor.y = WorldSky->Horizon.g;
	ShaderConst.horizonColor.z = WorldSky->Horizon.b;
	ShaderConst.horizonColor.w = 1.0f;

	ShaderConst.sunAmbient.x = WorldSky->sunAmbient.r;
	ShaderConst.sunAmbient.y = WorldSky->sunAmbient.g;
	ShaderConst.sunAmbient.z = WorldSky->sunAmbient.b;
	ShaderConst.sunAmbient.w = 1.0f;

	ShaderConst.skyLowColor.x = WorldSky->SkyLower.r;
	ShaderConst.skyLowColor.y = WorldSky->SkyLower.g;
	ShaderConst.skyLowColor.z = WorldSky->SkyLower.b;
	ShaderConst.skyLowColor.w = 1.0f;

	ShaderConst.skyColor.x = WorldSky->skyUpper.r;
	ShaderConst.skyColor.y = WorldSky->skyUpper.g;
	ShaderConst.skyColor.z = WorldSky->skyUpper.b;
	ShaderConst.skyColor.w = 1.0f;

	// replicate vanilla behavior of enforcing max fog distance in interiors
	ShaderConst.fogData.y = WorldSky->fogFarPlane;
	if (!GameState.isExterior && (WorldSky->fogFarPlane < 0 || WorldSky->fogFarPlane > 163840)) {
		ShaderConst.fogData.y = 163840;
	}

	// for near plane, ensure that far > near
	ShaderConst.fogData.x = WorldSky->fogNearPlane;
	if (WorldSky->fogNearPlane < 0 || ShaderConst.fogData.y < WorldSky->fogNearPlane)
		ShaderConst.fogData.x = ShaderConst.fogData.y * 0.17;

	ShaderConst.fogData.z = ShaderConst.sunGlare;
	ShaderConst.fogData.w = WorldSky->fogPower;

	ShaderConst.fogDistance.x = ShaderConst.fogData.x;
	ShaderConst.fogDistance.y = ShaderConst.fogData.y;
	ShaderConst.fogDistance.z = 1.0f;
	ShaderConst.fogDistance.w = ShaderConst.sunGlare;

	timer.LogTime("ShaderManager::UpdateConstants for generic constants");

	if (TheSettingManager->SettingsChanged) {
		// TheGameMenuManager->UpdateSettings(); — replaced by ImGui overlay

		// update settings
		for (const auto [Name, effect] : EffectsNames) {
			(*effect)->UpdateSettings();
		}
		for (const auto [Name, shader] : ShaderNames) {
			(*shader)->UpdateSettings();
		}

		// sky settings are used in several shaders whether the shader is active or not
		ShaderConst.SunAmount.w = TheSettingManager->GetSettingF("Shaders.Sky.Main", "GlareStrength");
		timer.LogTime("ShaderManager::UpdateSettings for shaders & effects");
	}

	// update Constants
	for (const auto [Name, shader] : ShaderNames) {
		if ((*shader)->Enabled) (*shader)->UpdateConstants();
	}
	timer.LogTime("ShaderManager::UpdateConstants for shaders");

	for (const auto [Name, effect] : EffectsNames) {
		if ((*effect)->Enabled) {
			(*effect)->UpdateConstants();
			(*effect)->constantUpdateTime = timer.LogTime((*effect)->Name);
		}
		else {
			(*effect)->constantUpdateTime = 0;
		}
	}

	// The skin, hair and grass shaders route their light and ambient terms through
	// TESR_PBRData (see Shaders/Includes/PBRScale.hlsl) and render black at a zero scale, so
	// these constants stay current whether or not the PBR collection is enabled.
	if (!Shaders.PBR->Enabled) Shaders.PBR->UpdateConstants();

	// Underwater effect uses constants from the water shader
	if (Effects.Underwater->Enabled && !Shaders.Water->Enabled) Shaders.Water->UpdateConstants();
	if (!Effects.ShadowsExteriors->Enabled && Effects.ShadowsInteriors->Enabled) Effects.ShadowsExteriors->UpdateConstants(); // Interior and exterior shadows share settings

	TheSettingManager->SettingsChanged = false;
	timer.LogTime("ShaderManager::UpdateConstants");
}


float ShaderManager::GetTransitionValue(float Day, float Night, float Interior) {
	if (GameState.isExterior) {
		return std::lerp(Night, Day, GameState.transitionCurve);
	}
	else {
		return Interior;
	}
}


ShaderCollection* ShaderManager::GetShaderCollection(const char* Name) {

	if (!memcmp(Name, "WATER", 5)) return Shaders.Water;
	if (!memcmp(Name, "GRASS", 5)) return Shaders.Grass;
	if (!memcmp(Name, "ISHDR", 5) || !memcmp(Name, "HDR", 3)) return Shaders.Tonemapping; // tonemapping shaders have different names between New vegas and Oblivion
	if (!memcmp(Name, "PAR", 3)) return Shaders.POM;
	if (!memcmp(Name, "SKIN", 4)) return Shaders.Skin;
	// Hair (BSSM_3XLIGHTING_*) lives in the SM3 family, not HAIR*. Only SM3003 has a
	// replacement on disk; the rest resolve to no file and fall through to vanilla.
	if (!memcmp(Name, "SM3", 3)) return Shaders.PBR;
	// SpeedTree leaves. STLEAF001/003.vso are vs_3_0 replacements, so every leaf PS must have
	// a ps_3_0 replacement too: D3D9 rejects a 2.x VS paired with a 3.0 PS.
	if (!memcmp(Name, "STLEAF", 6)) return Shaders.PBR;
	if (!memcmp(Name, "SKY", 3)) return Shaders.Sky;
	if (strstr(BloodShaders, Name)) return Shaders.Blood;

	if (Shaders.PBR->GetTemplate(Name).Name != NULL) return Shaders.PBR;
	if (Shaders.Terrain->GetTemplate(Name).Name != NULL) return Shaders.Terrain;

	return NULL;
}

/*
* Reload all effects.
*/
void ShaderManager::ReloadEffects() {
	for (const auto [Name, effect] : EffectsNames) {
		(*effect)->DisposeEffect();
		(*effect)->LoadEffect();
	}
}

/*
* Load generic Vertex Shaders as well as the ones for interiors and exteriors if the exist. 
* Returns false if generic one isn't found (as other ones are optional)
*/
bool ShaderManager::LoadShader(NiD3DVertexShader* Shader) {
	
	NiD3DVertexShaderEx* VertexShader = (NiD3DVertexShaderEx*)Shader;
	ShaderCollection* Collection = GetShaderCollection(VertexShader->Name);

	if (!Collection) {
		VertexShader->ShaderProg[ShaderRecordType::Default] = NULL;
		VertexShader->ShaderProg[ShaderRecordType::Exterior] = NULL;
		VertexShader->ShaderProg[ShaderRecordType::Interior] = NULL;
		VertexShader->Enabled = false;
		return false;
	}
	
	bool enabled = Collection->Enabled;

	ShaderTemplate Template = Collection->GetTemplate(VertexShader->Name);

	// Load generic, interior and exterior shaders
	VertexShader->ShaderProg[ShaderRecordType::Default]  = (ShaderRecordVertex*)ShaderRecord::LoadShader(VertexShader->Name, NULL, Template);
	VertexShader->ShaderProg[ShaderRecordType::Exterior] = (ShaderRecordVertex*)ShaderRecord::LoadShader(VertexShader->Name, "Exteriors\\", Template);
	VertexShader->ShaderProg[ShaderRecordType::Interior] = (ShaderRecordVertex*)ShaderRecord::LoadShader(VertexShader->Name, "Interiors\\", Template);
	VertexShader->Enabled = enabled;

	if (VertexShader->ShaderProg[ShaderRecordType::Default] != nullptr || VertexShader->ShaderProg[ShaderRecordType::Exterior] != nullptr || VertexShader->ShaderProg[ShaderRecordType::Interior] != nullptr) {
		Collection->VertexShaderList.push_back(VertexShader);
		Logger::Log("Loaded %s Vertex Shader %s", Collection->Name, VertexShader->Name);
	}

	return enabled;
}


/*
* Load generic Pixel Shaders as well as the ones for interiors and exteriors if the exist. 
* Returns false if generic one isn't found (as other ones are optional)
*/
bool ShaderManager::LoadShader(NiD3DPixelShader* Shader) {

	NiD3DPixelShaderEx* PixelShader = (NiD3DPixelShaderEx*)Shader;
	ShaderCollection* Collection = GetShaderCollection(PixelShader->Name);

	if (!Collection) {
		PixelShader->ShaderProg[ShaderRecordType::Default] = NULL;
		PixelShader->ShaderProg[ShaderRecordType::Exterior] = NULL;
		PixelShader->ShaderProg[ShaderRecordType::Interior] = NULL;
		PixelShader->Enabled = false;
		return false;
	}

	bool enabled = Collection->Enabled;

	ShaderTemplate Template = Collection->GetTemplate(PixelShader->Name);

	PixelShader->ShaderProg[ShaderRecordType::Default]  = (ShaderRecordPixel*)ShaderRecord::LoadShader(PixelShader->Name, NULL, Template);
	PixelShader->ShaderProg[ShaderRecordType::Exterior] = (ShaderRecordPixel*)ShaderRecord::LoadShader(PixelShader->Name, "Exteriors\\", Template);
	PixelShader->ShaderProg[ShaderRecordType::Interior] = (ShaderRecordPixel*)ShaderRecord::LoadShader(PixelShader->Name, "Interiors\\", Template);
	PixelShader->Enabled = enabled;

	if (PixelShader->ShaderProg[ShaderRecordType::Default] != nullptr || PixelShader->ShaderProg[ShaderRecordType::Exterior] != nullptr || PixelShader->ShaderProg[ShaderRecordType::Interior] != nullptr) {
		Collection->PixelShaderList.push_back(PixelShader);
		Logger::Log("Loaded %s Pixel Shader %s", Collection->Name, PixelShader->Name);
	}

	return enabled;
}


void ShaderManager::GetNearbyLights(ShadowSceneLight* ShadowLightsList[], NiPointLight* LightsList[], NiSpotLight* SpotLightList[]) {
	D3DXVECTOR4 PlayerPosition = Player->pos.toD3DXVEC4();
	//Logger::Log(" ==== Getting lights ====");
	auto timer = TimeLogger();

	// create a map of all nearby valid lights and sort them per distance to player
	std::map<int, ShadowSceneLight*> SceneLights;
	NiTList<ShadowSceneLight>::Entry* Entry = SceneNode->lights.start;

	ShadowsExteriorEffect::InteriorsStruct* Settings = &Effects.ShadowsExteriors->Settings.Interiors;
	ShadowsExteriorEffect::ShadowStruct* ShadowsConstants = &Effects.ShadowsExteriors->Constants;

	// Creating list of lights in order of distance to the player
	while (Entry) {
		NiPointLight* Light = Entry->data->sourceLight;
		D3DXVECTOR4 LightPosition = Light->m_worldTransform.pos.toD3DXVEC4();

		bool lightCulled = Light->m_flags & NiAVObject::NiFlags::APP_CULLED;
		bool lightOn = (Light->Diff.r + Light->Diff.g + Light->Diff.b) * Light->Dimmer > 5.0 / 255.0; // Check for low values in case of human error
		if (lightCulled || !lightOn) {
			Entry = Entry->next;
			continue;
		}

		D3DXVECTOR4 LightVector = LightPosition - PlayerPosition;
		D3DXVec4Normalize(&LightVector, &LightVector);
		bool inFront = D3DXVec4Dot(&LightVector, &TheRenderManager->CameraForward) > 0;
		float Distance = Light->GetDistance(&Player->pos);
		float radius = Light->Spec.r * Settings->LightRadiusMult;

		// select lights that will be tracked by removing culled lights and lights behind the player further away than their radius
		// TODO: handle using frustum check
		float drawDistance = 8000;//TheShaderManager->GameState.isExterior ? TheSettingManager->SettingsShadows.Exteriors.ShadowMapRadius[TheShadowManager->ShadowMapTypeEnum::MapLod] : TheSettingManager->SettingsShadows.Interiors.DrawDistance;
		if ((inFront || Distance < radius) && (Distance + radius) < drawDistance) {
			SceneLights[(int)(Distance * 10000)] = Entry->data; // multiplying distance (used as key) before conversion to avoid overwriting in case of similar values
		}

		Entry = Entry->next;
	}

	// save only the n first lights (based on #define TrackedLightsMax)
	memset(&TheShaderManager->LightPosition, 0, TrackedLightsMax * sizeof(D3DXVECTOR4)); // clear previous lights from array
	// Must be cleared. The fill loop below only zeroes trailing slots once it runs out of scene
	// lights; with more lights than slots it never reaches that branch, and a slot left holding
	// last frame's position keeps GetPointLightAmount sampling a cubemap nobody redraws.
	memset(&ShadowsConstants->ShadowLightPosition, 0, ShadowCubeMapsMax * sizeof(D3DXVECTOR4));
	memset(&TheShaderManager->LightColor, 0, (TrackedLightsMax + ShadowCubeMapsMax) * sizeof(D3DXVECTOR4)); // clear previous lights from array

	// ShadowManager::RenderShadowMaps only renders cubemaps for the first LightPoints slots.
	// Filling past that gives the shader a live position and colour for a face that is never
	// redrawn, so it samples whatever that cubemap last held -- a shadow frozen from an earlier
	// frame or cell. Lights beyond the cap fall through to the non-shadowing tracked list.
	const int ShadowLightsMax = min(Settings->LightPoints, (int)ShadowCubeMapsMax);

	// get the data for all tracked lights
	int ShadowIndex = 0;
	int LightIndex = 0;
	TheShadowManager->PointLightsNum = 0;
	ShadowSceneLight* ShadowCasters[ShadowCubeMapsMax] = { NULL }; // shadow casting lights, nearest first

#if defined(OBLIVION)
	bool TorchOnBeltEnabled = TheSettingManager->SettingsMain.EquipmentMode.Enabled && TheSettingManager->SettingsMain.EquipmentMode.TorchKey != 255;
#endif

	D3DXVECTOR4 Empty = D3DXVECTOR4(0, 0, 0, 0);

	// TEMP : get data for spotlights. Right now, is done manually since spotlights aren't implemented in the engine
	// FlashlightEffect::UpdateConstants publishes SpotLightPosition/Direction/Color itself,
	// so the cone constants and the cookie matrix always come from one read of the bone.
	TheShaderManager->Effects.Flashlight->UpdateConstants();
	if (TheShaderManager->Effects.Flashlight->Enabled && TheShaderManager->Effects.Flashlight->spotLightActive) {
		SpotLightList[0] = TheShaderManager->Effects.Flashlight->SpotLight;
	}
	else {
		SpotLightList[0] = nullptr;
	}

	std::map<int, ShadowSceneLight*>::iterator v = SceneLights.begin();
	for (int i = 0; i < TrackedLightsMax + ShadowCubeMapsMax; i++) {
		// set null values if we reached the end of lights in the scene and current index is lower than max amount
		if (v == SceneLights.end()) {
			// (Unused shadow slots stay cleared: they are assigned after this loop.)
			if (LightIndex < TrackedLightsMax) {
				//Logger::Log("clearing light at index %i", LightIndex);
				LightsList[LightIndex] = NULL;
				LightPosition[LightIndex] = Empty;
				LightColor[ShadowCubeMapsMax + LightIndex] = Empty;
				LightIndex++;
			}

			continue;
		}

		NiPointLight* Light = v->second->sourceLight;
		if (!Light) {
			v++;
			continue;
		}

		if (Light->EffectType == NiDynamicEffect::EffectTypes::POINT_LIGHT) {
			// determin if light is a shadow caster
			//bool CastShadow = Settings->UseCastShadowFlag ? Light->CastShadows : true; // Flag is broken by JIP
			bool CastShadow = true;

#if defined(OBLIVION)
			// Oblivion exception for carried torch lights 
			if (TorchOnBeltEnabled && Light->CanCarry == 2) {
				HighProcessEx* Process = (HighProcessEx*)Player->process;
				if (Process->OnBeltState == HighProcessEx::State::In) CastShadow = false;
		}
#endif
			float radius = Light->Spec.r * Settings->LightRadiusMult;
			D3DXVECTOR4 LightPos = Light->m_worldTransform.pos.toD3DXVEC4();
			LightPos.w = radius;

			if (CastShadow && ShadowIndex < ShadowLightsMax && radius > 10) {
				// add found light to the ranked list of lights that cast shadows (slots are assigned below)
				ShadowCasters[ShadowIndex] = v->second;

				ShadowIndex++;
				TheShadowManager->PointLightsNum++; // Constant to track number of shadow casting lights are present
			}
			else if (LightIndex < TrackedLightsMax) {
				LightsList[LightIndex] = Light;
				LightPosition[LightIndex] = LightPos;
				LightColor[ShadowCubeMapsMax + LightIndex] = D3DXVECTOR4(Light->Diff.r, Light->Diff.g, Light->Diff.b, Light->Dimmer);
				LightIndex++;
			};
		}
		else if (Light->EffectType == NiDynamicEffect::EffectTypes::SPOT_LIGHT) {
			// Here will go the collecting of the spotlights and setting of constants
		}
		v++;
	}

	// Give every caster a cubemap slot, each light keeping the slot it had last frame (PointShadowSlots.h): a light that
	// only changed rank no longer forces its cubemap to be redrawn. The slot order does not change the image, except for
	// the last slot, which PointShadows.fx lights without a shadow lookup: it keeps the farthest caster, as the plain
	// distance order gave it, and only slots 0..10 take part in the stable assignment.
	{
		static const void* previousSlots[ShadowCubeMapsMax] = {};
		const int casters = TheShadowManager->PointLightsNum;
		const int sampledSlots = min(ShadowLightsMax, (int)ShadowCubeMapsSampled);
		const int stableCasters = min(casters, sampledSlots);
		const void* ranked[ShadowCubeMapsMax] = {};
		const void* assigned[ShadowCubeMapsMax] = {};
		for (int r = 0; r < stableCasters; r++) ranked[r] = ShadowCasters[r];
		AssignStablePointShadowSlots(previousSlots, ranked, stableCasters, sampledSlots, assigned);
		if (casters > sampledSlots) assigned[ShadowCubeMapsMax - 1] = ShadowCasters[sampledSlots];
		for (int s = 0; s < ShadowCubeMapsMax; s++) previousSlots[s] = s < sampledSlots ? assigned[s] : nullptr;

		for (int s = 0; s < ShadowCubeMapsMax; s++) {
			ShadowSceneLight* shadowLight = (ShadowSceneLight*)assigned[s];
			ShadowLightsList[s] = shadowLight;
			if (!shadowLight) continue; // position and colour stay cleared
			NiPointLight* Light = shadowLight->sourceLight;
			D3DXVECTOR4 LightPos = Light->m_worldTransform.pos.toD3DXVEC4();
			LightPos.w = Light->Spec.r * Settings->LightRadiusMult;
			ShadowsConstants->ShadowLightPosition[s] = LightPos;
			LightColor[s] = D3DXVECTOR4(Light->Diff.r, Light->Diff.g, Light->Diff.b, Light->Dimmer);
		}
	}

	timer.LogTime("ShaderManager::GetNearbyLights");
}


// Quality 0/1 -> VSM, 2 -> EVSM2, 3 -> EVSM4, mirroring ShadowsExteriorEffect::
// UpdateSettingsFromQuality. Quality 4 (Custom), or an out-of-range value, reads Mode directly.
int ShaderManager::ShadowModeFromSettings() {
	switch (TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "Quality")) {
	case 0:
	case 1:
		return 0; // VSM
	case 2:
		return 1; // EVSM2
	case 3:
		return 2; // EVSM4
	default:
		return std::clamp(TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "Mode"), 0, 2);
	}
}


bool ShaderManager::ShouldRenderShadowMaps() {
	if (GameState.isExterior)
		return orthoRequired || (
			Effects.ShadowsExteriors->Settings.Exteriors.Enabled &&
			Effects.ShadowsExteriors->Enabled);
	else
		return Effects.ShadowsInteriors->Enabled;
}

/*
* Renders a given effect to an arbitrary render target
*/
void ShaderManager::RenderEffectToRT(IDirect3DSurface9* RenderTarget, EffectRecord* Effect, bool clearRenderTarget, UINT techniqueIndex) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	Device->SetRenderTarget(0, RenderTarget);
	Effect->Render(Device, RenderTarget, RenderTarget, techniqueIndex, clearRenderTarget, RenderTarget);
};


void ShaderManager::RenderEffectsPreTonemapping(IDirect3DSurface9* RenderTarget) {
	if (!TheSettingManager->SettingsMain.Main.RenderEffects) return; // Main toggle
	if (!Player->parentCell) return;
	if (GameState.OverlayIsOn && TESMain::IsMenuBackgroundReady()) return; // disable all effects during terminal/lockpicking sequences

	auto timer = TimeLogger();
	static CpuTimer preTonemapCpuTimer("Pre-tonemap chain (CPU)");
	CpuProfileScope preTonemapCpu(preTonemapCpuTimer);

	IDirect3DDevice9* Device = TheRenderManager->device;
	IDirect3DSurface9* SourceSurface = TheTextureManager->SourceSurface;
	IDirect3DSurface9* RenderedSurface = TheTextureManager->RenderedSurface;
	static GpuTimer depthTimer("Depth combine");
	static GpuTimer normalsTimer("Normal reconstruction");
	static GpuTimer pointShadowTimer("Point shadow lighting");
	static GpuTimer sunContactTimer("Sun contact shadows");
	static GpuTimer shadowApplyTimer("Shadow apply");
	static GpuTimer aoTimer("Ambient occlusion");
	static GpuTimer snowAccumulationTimer("Snow accumulation");
	static GpuTimer materialEffectsTimer("Wet + light materials");
	static GpuTimer volumetricLightTimer("Volumetric light");
	static GpuTimer fogTimer("Volumetric fog");
	static GpuTimer godRaysTimer("God rays");
	static GpuTimer hdrTimer("Luma + exposure + bloom");
	static GpuTimer avgLumaTimer("  Average luma");
	static GpuTimer exposureTimer("  Exposure");
	static GpuTimer bloomTimer("  Bloom buffers");
	static GpuTimer preColorTimer("Pre-tonemap lens + LUT");

	// prepare device for effects
	Device->SetStreamSource(0, FrameVertex, 0, sizeof(FrameVS));
	Device->SetFVF(FrameFVF);

	// render post process normals for use by shaders
	// When the normals pass would run anyway, try producing depth and normals in one draw.
	bool mergedNormals = false;
	{
		GpuProfileScope gpu(depthTimer, Device);
		NormalsEffect* Normals = Effects.Normals;
		if (Normals->Enabled && Normals->Effect && Normals->ShouldRender())
			mergedNormals = Effects.CombineDepth->RenderWithNormals(Device, Normals->Textures.NormalsSurface);
		if (!mergedNormals)
			RenderEffectToRT(Effects.CombineDepth->Textures.CombinedDepthSurface, Effects.CombineDepth, false);
	}
	{
		GpuProfileScope gpu(normalsTimer, Device);
		if (!mergedNormals)
			RenderEffectToRT(Effects.Normals->Textures.NormalsSurface, Effects.Normals, false);
	}

	// render a shadow pass for point lights
	if ((GameState.isExterior && Effects.ShadowsExteriors->Enabled) || (!GameState.isExterior && Effects.ShadowsInteriors->Enabled)) {
		{
			GpuProfileScope gpu(pointShadowTimer, Device);
			RenderEffectToRT(Effects.ShadowsExteriors->Textures.ShadowPassSurface, Effects.PointShadows, true);
			// The stock/custom shader remains compatible: it has no named merged
			// technique, so lights 6-11 still take the original second pass.
			const bool mergedPointShadows = Effects.PointShadows->Effect &&
				Effects.PointShadows->Effect->GetTechniqueByName("MergedPointShadows") != NULL;
			if (!mergedPointShadows && Effects.ShadowsExteriors->Settings.Interiors.LightPoints > 6)
				RenderEffectToRT(Effects.ShadowsExteriors->Textures.ShadowPassSurface, Effects.PointShadows2, false);
		}
		if (GameState.isExterior) {
			{
				GpuProfileScope gpu(sunContactTimer, Device);
				// While the forward path runs, Shadow() only raises the blurred contact shadows to
				// the configured intensity, so the named ForwardContactShadows technique does that in
				// the vertical blur and saves a pass. It is looked up by name: the technique indices
				// after 0 belong to the temporal filter's techniques.
				UINT sunTechnique = 0;
				if (Effects.ShadowsExteriors->ForwardShadowsRunning() && Effects.SunShadows->Effect) {
					ID3DXEffect* sunEffect = Effects.SunShadows->Effect;
					D3DXHANDLE fusedTechnique = sunEffect->GetTechniqueByName("ForwardContactShadows");
					D3DXEFFECT_DESC sunDesc;
					if (fusedTechnique && SUCCEEDED(sunEffect->GetDesc(&sunDesc))) {
						for (UINT i = 0; i < sunDesc.Techniques; i++) {
							if (sunEffect->GetTechnique(i) == fusedTechnique) {
								sunTechnique = i;
								break;
							}
						}
					}
				}
				RenderEffectToRT(Effects.ShadowsExteriors->Textures.ShadowPassSurface, Effects.SunShadows, false, sunTechnique);
			}
			// The screen-space buffer's temporal filter. Deferred path only: on the forward one the
			// buffer holds just the contact shadows.
			if (Effects.ShadowsExteriors->Settings.ShadowMaps.TemporalFilter && !Effects.ShadowsExteriors->ForwardShadowsRunning())
				RenderEffectToRT(Effects.ShadowsExteriors->Textures.ShadowPassSurface, Effects.SunShadows, false, 2);
			// The forward path's filtered cascade term, for next frame's object shaders.
			if (Effects.ShadowsExteriors->ForwardTemporalActive())
				RenderEffectToRT(Effects.ShadowsExteriors->Textures.ForwardBufferSurface, Effects.SunShadows, false, 1);
			Effects.ShadowsExteriors->UpdateTemporalHistory();
		}
	}
	else {
		// Nothing above ran this frame, so ShadowPassSurface keeps whatever it last
		// held -- e.g. an exterior sun-shadow composite, walked in from outdoors,
		// frozen here for as long as this branch keeps being skipped (interior with
		// Interior point-shadows off is the common case). It's still sampled
		// unconditionally by other independently-enabled effects (Specular and
		// others), so reset it to the neutral "no shadow" value rather than leaving
		// stale exterior data for them to read.
		Effects.ShadowsExteriors->clearShadowsBuffer();
	}

	Device->SetRenderTarget(0, RenderTarget);

	// Start the copy-free chain (FrameChain), or seed the rendered texture for the legacy path in
	// which every effect keeps it equal to the render target. TESR_SourceBuffer is refreshed by
	// each effect that actually samples it (EffectRecord::usesSourceBuffer). Effects are handed
	// TheTextureManager->RenderedSurface at call time because the chain swaps it.
	struct ChainGuard { ~ChainGuard() { TheShaderManager->Chain.End(); } } chainGuard;
	if (!Chain.Begin(RenderTarget))
		Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);

	// Composite apply: the exterior sun-shadow composite and the AO combine are per-pixel
	// operations on the scene, applied just before fog. When no effect that normally runs between
	// them would render this frame, the fog reconstruct applies them in the same pass (same maths,
	// same order), saving two full-resolution read/write passes of the HDR frame.
	auto wouldRender = [](EffectRecord* effect) { return effect && effect->Enabled && effect->Effect && effect->ShouldRender(); };
	AmbientOcclusionEffect* AO = Effects.AmbientOcclusion;
	VolumetricFogEffect* Fog = Effects.VolumetricFog;
	const bool shadowApplies = GameState.isExterior && wouldRender(Effects.ShadowsExteriors);
	const bool aoApplies = wouldRender(AO);
	const bool effectsBetween = wouldRender(Effects.SnowAccumulation) || wouldRender(Effects.WetWorld) ||
		wouldRender(Effects.Flashlight) || wouldRender(Effects.Specular) || wouldRender(Effects.Underwater) ||
		wouldRender(Effects.VolumetricLight);
	bool composite = (shadowApplies || aoApplies) && !effectsBetween &&
		Fog->CanComposite(aoApplies ? AO->NextResultSurface() : nullptr);
	AO->deferredReady = false;

	{
		GpuProfileScope gpu(shadowApplyTimer, Device);
		if (GameState.isExterior) {
			if (!(composite && shadowApplies))
				Effects.ShadowsExteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		}
		else
			Effects.ShadowsInteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, true, SourceSurface);
	}

	{
		GpuProfileScope gpu(snowAccumulationTimer, Device);
		Effects.SnowAccumulation->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(aoTimer, Device);
		AO->deferCombine = composite && aoApplies;
		AO->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		AO->deferCombine = false;
		if (composite && aoApplies && !AO->deferredReady) {
			// Dedicated AO was unavailable or failed. Restore the original order before
			// its legacy path reads the scene for luminance-dependent AO strength.
			composite = false;
			if (shadowApplies)
				Effects.ShadowsExteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
			AO->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		}
	}
	{
		GpuProfileScope gpu(materialEffectsTimer, Device);
		Effects.WetWorld->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		// Beam march first, into its own half res buffer, so the Flashlight Combine pass can
		// read it. Control.x already folds the effect toggle, the per view toggle and the
		// strength together, so this one test gates the whole thing.
		if (Effects.FlashlightBeam->Constants.Control.x > 0.0f) {
			RenderEffectToRT(Effects.FlashlightBeam->Textures.VolumetricSurface, Effects.FlashlightBeam, true);
			Device->SetRenderTarget(0, RenderTarget);
		}
		Effects.Flashlight->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, Effects.Flashlight->selectedPass, true, SourceSurface);
		Effects.Specular->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		Effects.Underwater->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		// VolumetricLight (upstream #78): the march into its own half-res buffer (technique 0) and its temporal
		// filter, then the composite onto the scene (technique 1), just before fog as upstream orders it. The march
		// does not read the scene, so running it here instead of before the flashlight gives the same result. The
		// guard is upstream's: RenderEffectToRT switches the target before Render can test Enabled/ShouldRender.
		GpuProfileScope gpu(volumetricLightTimer, Device);
		VolumetricLightEffect* light = Effects.VolumetricLight;
		if (light->Textures.VolumetricSurface && light->Enabled && light->ShouldRender()) {
			RenderEffectToRT(light->Textures.VolumetricSurface, light, true);
			light->RenderTemporal(Device);
			Device->SetRenderTarget(0, RenderTarget);
		}
		light->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 1, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(fogTimer, Device);
		Fog->compositeShadow = composite && shadowApplies;
		Fog->compositeAO = composite && AO->deferredReady;
		Fog->compositeAOTexture = AO->ResultTexture();
		Fog->compositeApplied = false;
		Fog->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		const bool applied = Fog->compositeApplied;
		Fog->compositeShadow = Fog->compositeAO = Fog->compositeApplied = false;
		if (composite && !applied) {
			// Failed composite fog leaves the scene untouched. Restore shadows -> AO -> fog.
			if (shadowApplies)
				Effects.ShadowsExteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
			if (AO->deferredReady) {
				AO->combineOnly = true;
				AO->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
				AO->combineOnly = false;
			}
			Fog->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		}
		AO->deferredReady = false;
	}
	{
		GpuProfileScope gpu(godRaysTimer, Device);
		Effects.GodRays->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, true, SourceSurface);
	}

	{
		GpuProfileScope gpu(hdrTimer, Device);
		// calculate average luma for use by shaders
		if (avglumaRequired) {
			GpuProfileScope gpuLuma(avgLumaTimer, Device);
			RenderEffectToRT(Effects.AvgLuma->Textures.AvgLumaSurface, Effects.AvgLuma, NULL);
			Device->SetRenderTarget(0, RenderTarget); 	// restore device used for effects
		}
		{
			GpuProfileScope gpuExposure(exposureTimer, Device);
			Effects.Exposure->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		}
		{
			GpuProfileScope gpuBloom(bloomTimer, Device);
			Effects.Bloom->RenderBloomBuffer(RenderTarget);
		}
	}

	{
		GpuProfileScope gpu(preColorTimer, Device);
		Effects.Lens->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		if (Effects.LUT->Settings.PreTonemapping)
			Effects.LUT->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}

	timer.LogTime("ShaderManager::RenderEffectsPreTonemapping");
}


/*
* Renders the effect that have been set to enabled.
*/
void ShaderManager::RenderEffects(IDirect3DSurface9* RenderTarget) {
	// F10 profiling toggle and the frame interval run before the RenderEffects check, so an
	// effects-off run still logs its real frame time for comparison. This is the last NVR call
	// of the frame, after the pre-tonemap chain, so the toggle takes effect from the next frame.
	static CpuTimer frameIntervalTimer("Frame interval (CPU)");
	if (Player->parentCell && !InterfaceManager->IsActive(Menu::kMenuType_Loading) && Global->OnKeyDown(0x44)) {
		GpuTimer::Enabled = !GpuTimer::Enabled;
		Logger::Log("GPU PROFILE %s (F10), effects %s, D3D9 runtime: %s", GpuTimer::Enabled ? "enabled" : "paused",
			TheSettingManager->SettingsMain.Main.RenderEffects ? "on" : "OFF", TheRenderManager->D3D9RuntimeDescription());
		if (!GpuTimer::Enabled) TheFrameTimeMonitor().Flush(); // report the frames collected so far
	}
	if (GpuTimer::Enabled) {
		// Frames within three seconds of a cell change or loading screen are counted separately: the
		// hitches there are expected (streaming), the ones in steady play are what hurt the 1% lows.
		static TESObjectCELL* lastCell = nullptr;
		static unsigned framesSinceTransition = 1000;
		TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
		if (cell != lastCell || InterfaceManager->IsActive(Menu::kMenuType_Loading)) { lastCell = cell; framesSinceTransition = 0; }
		else framesSinceTransition++;
		const double interval = frameIntervalTimer.Tick();
		if (interval > 0.0) TheFrameTimeMonitor().Add(interval, framesSinceTransition < 180);
	}
	if (!TheSettingManager->SettingsMain.Main.RenderEffects) return; // Main toggle
	if (!Player->parentCell) return;
	if (GameState.OverlayIsOn) return; // disable all effects during terminal/lockpicking sequences because they bleed through the overlay

	auto timer = TimeLogger();
	static CpuTimer postChainCpuTimer("Post chain (CPU)");
	CpuProfileScope postChainCpu(postChainCpuTimer);

	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();

	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	IDirect3DSurface9* SourceSurface = TheTextureManager->SourceSurface;
	IDirect3DSurface9* RenderedSurface = TheTextureManager->RenderedSurface;
	static GpuTimer postCopyTimer("Post-tonemap copies");
	static GpuTimer taaTimer("TAA");
	static GpuTimer weatherTimer("Weather + legacy bloom");
	static GpuTimer colorTimer("Coloring + LUT");
	static GpuTimer dofTimer("Depth of field");
	static GpuTimer motionBlurTimer("Motion blur");
	static GpuTimer lensTimer("Lens overlays");
	static GpuTimer ditherTimer("Dither buster");
	static GpuTimer smaaTimer("SMAA");
	static GpuTimer fxaaTimer("FXAA");
	static GpuTimer sharpenTimer("Sharpening");
	static GpuTimer cinemaTimer("Cinema");
	static GpuTimer imageAdjustTimer("Image adjust + debug");

	Device->SetStreamSource(0, FrameVertex, 0, sizeof(FrameVS));
	Device->SetFVF(FrameFVF);

	// prepare device for effects
	Device->SetRenderTarget(0, RenderTarget);

	// copy the source render target to both the rendered and source textures (rendered gets updated after every pass, source once per effect)
	{
		GpuProfileScope gpu(postCopyTimer, Device);
		if (!Chain.Begin(RenderTarget))
			Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	}
	struct ChainGuard { ~ChainGuard() { TheShaderManager->Chain.End(); } } chainGuard;

	// Name the effect that will render last so its final pass writes the game target directly and
	// the chain ends without a copy (FrameChain::SetFinalEffect). Same order as the calls below; the
	// tests are the ones EffectRecord::Render applies. A wrong guess is repaired in FrameChain::Owns.
	{
		EffectRecord* const order[] = {
			Effects.Rain, Effects.Snow, Effects.BloomLegacy, Effects.Coloring, Effects.LUT, Effects.DepthOfField,
			Effects.MotionBlur, Effects.BloodLens, Effects.WaterLens, Effects.LowHF, Effects.DitherBuster,
			Effects.SMAA, Effects.FXAA, Effects.Sharpening, Effects.Cinema, Effects.ImageAdjust, Effects.Debug };
		EffectRecord* last = nullptr;
		for (EffectRecord* effect : order)
			if (effect && effect->Enabled && effect->Effect && effect->ShouldRender()) last = effect;
		Chain.SetFinalEffect(last);
	}

	// TAA (upstream #78) first: after tonemapping, so it resolves LDR values that cannot ghost as HDR highlights
	// do, and ahead of everything below. Rain and snow are particles with no depth of their own to reproject by,
	// DoF and motion blur want the stable image as input, and the lens effects and cinema overlay are fixed to
	// the screen. TAAEffect::Render manages its own targets and syncs the frame chain first.
	{
		GpuProfileScope gpu(taaTimer, Device);
		Effects.TAA->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}

	{
		GpuProfileScope gpu(weatherTimer, Device);
		Effects.Rain->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		Effects.Snow->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		Effects.BloomLegacy->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}

	// screenspace coloring/blurring effects get rendered last
	{
		GpuProfileScope gpu(colorTimer, Device);
		Effects.Coloring->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		if (!Effects.LUT->Settings.PreTonemapping)
			Effects.LUT->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(dofTimer, Device);
		// Distant blur does not need the six-pass autofocus/bokeh pipeline.
		const UINT technique = !Effects.DepthOfField->Constants.Enabled && Effects.DepthOfField->Constants.Blur.x ? 1 : 0;
		// The distant-only technique reads TESR_RenderedBuffer and never TESR_SourceBuffer, so it
		// skips the full-resolution copy the full pipeline needs.
		Effects.DepthOfField->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, technique, false, technique == 1 ? nullptr : SourceSurface);
	}
	{
		GpuProfileScope gpu(motionBlurTimer, Device);
		Effects.MotionBlur->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}

	// lens effects
	{
		GpuProfileScope gpu(lensTimer, Device);
		Effects.BloodLens->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		Effects.WaterLens->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		Effects.LowHF->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(ditherTimer, Device);
		Effects.DitherBuster->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(smaaTimer, Device);
		Effects.SMAA->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(fxaaTimer, Device);
		Effects.FXAA->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(sharpenTimer, Device);
		Effects.Sharpening->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(cinemaTimer, Device);
		Effects.Cinema->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}
	{
		GpuProfileScope gpu(imageAdjustTimer, Device);
		Effects.ImageAdjust->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
		Effects.Debug->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface);
	}

	timer.LogTime("ShaderManager::RenderEffects");
}

EffectRecord* ShaderManager::GetEffectByName(const char* Name) {
	// effects
	EffectsList::iterator t = EffectsNames.find(Name);
	if (t == EffectsNames.end()) return nullptr;
	return *(t->second);
}


ShaderCollection* ShaderManager::GetShaderCollectionByName(const char* Name) {
	// shaders
	ShaderList::iterator t = ShaderNames.find(Name);
	if (t == ShaderNames.end()) return nullptr;
	return *(t->second);
}

/*
* Writes the settings corresponding to the shader/effect name, to switch it between enabled/disabled.*
* Also creates or deletes the corresponding Effect Record.
*/
void ShaderManager::SwitchShaderStatus(const char* Name) {
	IsMenuSwitch = true;

	// effects
	EffectRecord* effect = GetEffectByName(Name);
	if (effect) {
		bool setting = effect->SwitchEffect();
		TheSettingManager->SetMenuShaderEnabled(Name, setting);

		IsMenuSwitch = false;
		return;
	}

	// shaders
	ShaderCollection* shader = GetShaderCollectionByName(Name);
	if (shader) {
		bool setting = shader->SwitchShader();
		TheSettingManager->SetMenuShaderEnabled(Name, setting);

		IsMenuSwitch = false;
		return;
	}
}

void ShaderManager::SetCustomConstant(const char* Name, D3DXVECTOR4 Value) {
	CustomConstants::iterator v = CustomConst.find(std::string(Name));
	if (v != CustomConst.end()) v->second = Value;
}


bool FrameChain::Owns(IDirect3DSurface9* renderTarget, IDirect3DSurface9* renderedSurface) {
	const bool owns = Active && renderTarget == GameTarget && renderedSurface == TheTextureManager->RenderedSurface;
	if (owns) ReclaimFinal();
	return owns;
}

// The predicted last effect wrote the finished image into the game target, but another effect is
// about to render after it. Put the image back where the chain expects it (Surf[Current]) so that
// effect samples the right thing; a wrong prediction costs one copy instead of a wrong image.
void FrameChain::ReclaimFinal() {
	if (!FinalWritten) return;
	TheRenderManager->device->StretchRect(GameTarget, NULL, Surf[Current], NULL, D3DTEXF_NONE);
	FinalWritten = false;
	FinalEffect = nullptr;
}

// Point the TESR_RenderedBuffer slot at the current image. Effect samplers follow the slot
// (TextureRecord::TextureRef), so this is all a swap needs.
void FrameChain::Publish() {
	TheTextureManager->RenderedTexture = Tex[Current];
	TheTextureManager->RenderedSurface = Surf[Current];
}

bool FrameChain::EnsureTexture(Pair& pair, int slot) {
	if (pair.Texture[slot]) return true;
	if (FAILED(TheRenderManager->device->CreateTexture(pair.Width, pair.Height, 1, D3DUSAGE_RENDERTARGET, pair.Format,
		D3DPOOL_DEFAULT, &pair.Texture[slot], NULL)) || FAILED(pair.Texture[slot]->GetSurfaceLevel(0, &pair.Surface[slot]))) {
		if (pair.Surface[slot]) { pair.Surface[slot]->Release(); pair.Surface[slot] = nullptr; }
		if (pair.Texture[slot]) { pair.Texture[slot]->Release(); pair.Texture[slot] = nullptr; }
		Logger::Log("[ERROR] Frame chain: could not create a %ux%u target (format %u); using copies.", pair.Width, pair.Height, pair.Format);
		return false;
	}
	return true;
}

bool FrameChain::Begin(IDirect3DSurface9* gameTarget) {
	if (Active) End();
	FinalEffect = nullptr;
	FinalWritten = false;
	if (!gameTarget || TheSettingManager->SettingsMain.Main.DisableFrameChain) return false;
	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DSURFACE_DESC desc = {};
	if (FAILED(gameTarget->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) return false;

	// One set of NVR textures per target format (pre-tonemap HDR and post-tonemap LDR).
	int index = -1;
	for (int i = 0; i < 2 && index < 0; i++) {
		const Pair& pair = Pairs[i];
		if (pair.Format == desc.Format && pair.Width == desc.Width && pair.Height == desc.Height) index = i;
	}
	for (int i = 0; i < 2 && index < 0; i++) {
		if (Pairs[i].Format == D3DFMT_UNKNOWN) {
			Pairs[i].Format = desc.Format;
			Pairs[i].Width = desc.Width;
			Pairs[i].Height = desc.Height;
			index = i;
		}
	}
	if (index < 0) return false; // a third format: stay on the legacy path rather than churn
	Pair& pair = Pairs[index];

	// Use the game target itself as one buffer when it is a texture level (not the back buffer).
	// Single-level only: NVR's own buffers have no mip chain, and effects that sample the rendered
	// buffer at reduced size (average luma, bloom downsample) must not pick up stale mip levels.
	IDirect3DTexture9* gameTexture = nullptr;
	const bool useGameTexture = !TheSettingManager->SettingsMain.Main.DisableChainGameTexture &&
		SUCCEEDED(gameTarget->GetContainer(IID_IDirect3DTexture9, (void**)&gameTexture)) && gameTexture &&
		gameTexture->GetLevelCount() == 1;
	if (useGameTexture) {
		if (!EnsureTexture(pair, 0)) { gameTexture->Release(); return false; }
		Tex[0] = gameTexture;	Surf[0] = gameTarget;
		Tex[1] = pair.Texture[0]; Surf[1] = pair.Surface[0];
		GameTexture = gameTexture;
	}
	else {
		if (gameTexture) { gameTexture->Release(); gameTexture = nullptr; }
		if (!EnsureTexture(pair, 0) || !EnsureTexture(pair, 1)) return false;
		if (FAILED(Device->StretchRect(gameTarget, NULL, pair.Surface[0], NULL, D3DTEXF_NONE))) return false;
		Tex[0] = pair.Texture[0]; Surf[0] = pair.Surface[0];
		Tex[1] = pair.Texture[1]; Surf[1] = pair.Surface[1];
	}

	static bool reported[2][2] = {};
	if (!reported[index][useGameTexture]) {
		Logger::Log("Frame chain: %ux%u format %u, %s.", desc.Width, desc.Height, desc.Format,
			useGameTexture ? "game target used as a chain buffer (no seed copy)" : "two NVR buffers (seed copy)");
		reported[index][useGameTexture] = true;
	}

	Current = 0;
	SavedTexture = TheTextureManager->RenderedTexture;
	SavedSurface = TheTextureManager->RenderedSurface;
	GameTarget = gameTarget;
	Active = true;
	Publish();
	return true;
}

void FrameChain::Commit() {
	if (!Active) return;
	Current ^= 1;
	Publish();
}

void FrameChain::Sync() {
	if (!Active) return;
	ReclaimFinal();
	IDirect3DDevice9* Device = TheRenderManager->device;
	if (Surf[Current] == GameTarget) {
		// The game target already holds the image; move the current image to the other buffer
		// so the legacy path does not sample the texture it renders into.
		Device->StretchRect(GameTarget, NULL, Surf[Current ^ 1], NULL, D3DTEXF_NONE);
		Current ^= 1;
		Publish();
	}
	else
		Device->StretchRect(Surf[Current], NULL, GameTarget, NULL, D3DTEXF_NONE);
}

void FrameChain::End() {
	if (!Active) return;
	IDirect3DDevice9* Device = TheRenderManager->device;
	{
		// Shows whether a chain ends on the wrong buffer (~0 ms when it does not): the HDR chain (game
		// texture) copies when its pass count is odd, the LDR chain (back buffer) unless its last pass
		// wrote the game target itself (FinalWritten).
		static GpuTimer endCopyTimers[2] = { GpuTimer("  Chain end copy (HDR)"), GpuTimer("  Chain end copy (LDR)") };
		GpuProfileScope gpu(endCopyTimers[GameTexture ? 0 : 1], Device);
		if (Surf[Current] != GameTarget && !FinalWritten)
			Device->StretchRect(Surf[Current], NULL, GameTarget, NULL, D3DTEXF_NONE);
	}
	FinalEffect = nullptr;
	FinalWritten = false;
	Device->SetRenderTarget(0, GameTarget);
	TheTextureManager->RenderedTexture = SavedTexture;
	TheTextureManager->RenderedSurface = SavedSurface;
	if (GameTexture) { GameTexture->Release(); GameTexture = nullptr; }
	Tex[0] = Tex[1] = nullptr;
	Surf[0] = Surf[1] = nullptr;
	Active = false;
	GameTarget = nullptr;
}
