#pragma once
#define FrameFVF D3DFVF_XYZ | D3DFVF_TEX1

#include "../Effects/Animator.h"
#include "ShaderRecord.h"
#include "EffectRecord.h"
#include "ShaderCollection.h"
#include "../Effects/Effects.h"

struct ShaderConstants {
	
	struct OcclusionMapStruct {
		D3DXMATRIX		OcclusionWorldViewProj;
	};

	D3DXMATRIXA16			ShadowWorld;
	D3DXVECTOR4				ReciprocalResolution;
	D3DXVECTOR4				SunDir;
	D3DXVECTOR4				SunPosition;
	D3DXVECTOR4				SunTiming;
	D3DXVECTOR4				SunAmount;
	D3DXVECTOR4				ViewSpaceLightDir;
	D3DXVECTOR4				ScreenSpaceLightDir;
	D3DXVECTOR4				GameTime;
	D3DXVECTOR4				FrameTime;
	TESWeather*				pWeather;
	float					sunGlare;
	float					windSpeed;
	D3DXVECTOR4				fogColor;
	D3DXVECTOR4				horizonColor;
	D3DXVECTOR4				skyLowColor;
	float					skyObjectID;
	D3DXVECTOR4				sunDiskColor;
	D3DXVECTOR4				sunColor;
	D3DXVECTOR4				sunAmbient;
	D3DXVECTOR4				skyColor;
	D3DXVECTOR4				fogData;
	D3DXVECTOR4				fogDistance;
	float					fogStart;
	float					fogEnd;
	float					fogPower;
	OcclusionMapStruct		OcclusionMap;
};


typedef std::map<std::string, EffectRecord**> EffectsList;
typedef std::map<std::string, ShaderCollection**> ShaderList;
typedef std::map<std::string, D3DXVECTOR4> CustomConstants;

struct		FrameVS { float x, y, z, u, v; };

/*
* Copy-free effect chain. Without it every effect renders into the game's target and then copies
* the whole frame into TESR_RenderedBuffer so the next effect can sample it. With it, effects
* render into whichever of two chain textures is not the current image and the pair is swapped:
* TheTextureManager->RenderedTexture/Surface always name the current image, and the game's target
* holds the final image when the chain ends. The buffers match the game target's format, so
* precision and clamping between effects are unchanged.
*
* When the game target is itself a texture (the usual case), it is one of the two buffers: no
* seed copy at the start, and a copy at the end only when the last effect wrote the spare.
* Otherwise both buffers are NVR textures, seeded from and copied back to the game target.
*/
class FrameChain {
public:
	bool					Begin(IDirect3DSurface9* gameTarget); // false: chain unavailable, legacy behaviour
	void					End();
	bool					IsActive() const { return Active; }
	// True when an effect's (RenderTarget, RenderedSurface) pair is the chain's. Every effect that
	// renders through the chain asks this first, which is also where a pending direct-to-target final
	// pass (below) is undone if another effect turns out to render after it.
	bool					Owns(IDirect3DSurface9* renderTarget, IDirect3DSurface9* renderedSurface);
	IDirect3DSurface9*		Output() const { return Surf[Current ^ 1]; }
	void					Commit(); // the Output() surface becomes the current image
	// For legacy paths that render into the game target while sampling TESR_RenderedBuffer: the
	// game target gets the current image, and the current image is moved off the game target.
	void					Sync();

	// A chain on NVR-owned buffers (the game target is not one of them, as with the post-tonemap
	// back buffer) normally ends by copying the current image into the game target. If the caller
	// names the effect that renders last, that effect's final pass renders into the game target
	// itself and End() has nothing to copy.
	void					SetFinalEffect(const void* effect) { FinalEffect = effect; }
	bool					IsDirectFinal(const void* effect) const { return Active && !GameTexture && GameTarget && FinalEffect == effect; }
	IDirect3DSurface9*		FinalSurface() const { return GameTarget; }
	void					CommitFinal() { FinalWritten = true; } // the game target holds the finished image

private:
	void					ReclaimFinal();
	struct Pair { // NVR-owned textures for one target format/size, created on first use
		D3DFORMAT			Format = D3DFMT_UNKNOWN;
		UINT				Width = 0, Height = 0;
		IDirect3DTexture9*	Texture[2] = {};
		IDirect3DSurface9*	Surface[2] = {};
	};
	bool					EnsureTexture(Pair& pair, int slot);
	void					Publish();

	Pair					Pairs[2];
	IDirect3DTexture9*		Tex[2] = {};	// working buffers for the active chain
	IDirect3DSurface9*		Surf[2] = {};
	IDirect3DTexture9*		GameTexture = nullptr; // container of the game target, when used as Tex[0]
	int						Current = 0;
	bool					Active = false;
	IDirect3DSurface9*		GameTarget = nullptr;
	IDirect3DTexture9*		SavedTexture = nullptr;
	IDirect3DSurface9*		SavedSurface = nullptr;
	const void*				FinalEffect = nullptr;	// effect expected to render last in this chain
	bool					FinalWritten = false;	// its last pass already wrote the game target
};

__declspec(align(16)) class ShaderManager : public ShaderManagerBase { // Never disposed
public:
	static void Initialize();

	void* operator new(size_t i) { return _mm_malloc(i, 16); }

	template <typename T> void RegisterEffect(T** Pointer);
	template <typename T> void RegisterShaderCollection(T** Pointer);
	EffectRecord*			GetEffectByName(const char* Name);
	ShaderCollection*		GetShaderCollectionByName(const char* Name);
	void					ClearShaderSamplers(const char* TextureName, size_t Length);
	void					RegisterConstant(const char* Name, D3DXVECTOR4* FloatValue);
	void					CreateFrameVertex(UInt32 Width, UInt32 Height, IDirect3DVertexBuffer9** FrameVertex);
	void					InitializeConstants();
	void					UpdateConstants();
	void					GetNearbyLights(ShadowSceneLight* ShadowLightsList[], NiPointLight* LightsList[], NiSpotLight* SpotLightList[]);
	bool					LoadShader(NiD3DVertexShader* VertexShader);
	bool					LoadShader(NiD3DPixelShader* PixelShader);
	void					ReloadEffects();
	ShaderCollection*		GetShaderCollection(const char* Name);
	float					GetTransitionValue(float Day, float Night, float Interior);
	bool					ShouldRenderShadowMaps();
	void					RenderEffects(IDirect3DSurface9* RenderTarget);
	void					RenderEffectsPreTonemapping(IDirect3DSurface9* RenderTarget);
	void					RenderEffectToRT(IDirect3DSurface9* RenderTarget, EffectRecord* Effect, bool clearRenderTarget, UINT techniqueIndex = 0);
	void					SwitchShaderStatus(const char* Name);
	void					SetCustomConstant(const char* Name, D3DXVECTOR4 Value);
		
	struct	EffectsStruct {
		AmbientOcclusionEffect*	AmbientOcclusion;
		AvgLumaEffect*			AvgLuma;
		BloodLensEffect*		BloodLens;
		BloomEffect*			Bloom;
		BloomLegacyEffect*		BloomLegacy;
		ColoringEffect*			Coloring;
		LUTEffect*				LUT;
		CinemaEffect*			Cinema;
		ExposureEffect*			Exposure;
		FlashlightEffect*		Flashlight;
		FlashlightBeamEffect*	FlashlightBeam;
		CombineDepthEffect*		CombineDepth;
		DepthOfFieldEffect*		DepthOfField;
		DebugEffect*			Debug;
		GodRaysEffect*			GodRays;
		ImageAdjustEffect*		ImageAdjust;
		LensEffect*				Lens;
		LowHFEffect*			LowHF;
		MotionBlurEffect*		MotionBlur;
		NormalsEffect*			Normals;
		RainEffect*				Rain;
		SharpeningEffect*		Sharpening;
		SpecularEffect*			Specular;
		SnowEffect*				Snow;
		SnowAccumulationEffect*	SnowAccumulation;
		ShadowsExteriorEffect*	ShadowsExteriors;
		ShadowsInteriorsEffect*	ShadowsInteriors;
		PointShadowsEffect*		PointShadows;
		PointShadows2Effect*	PointShadows2;
		SunShadowsEffect*		SunShadows;
		UnderwaterEffect*		Underwater;
		VolumetricLightEffect*	VolumetricLight;
		VolumetricFogEffect*	VolumetricFog;
		WaterLensEffect*		WaterLens;
		WetWorldEffect*			WetWorld;
		DitherBusterEffect*		DitherBuster;
		SMAAEffect*				SMAA;
		FXAAEffect*				FXAA;
		TAAEffect*				TAA;
	};

	struct ShadersStruct{
		WaterShaders*			Water;
		TonemappingShaders*		Tonemapping;
		POMShaders*				POM;
		PBRShaders*				PBR;
		ShaderCollection*		Blood;
		SkyShaders*				Sky;
		SkinShaders*			Skin;
		GrassShaders*			Grass;
		TerrainShaders*			Terrain;
	};

	struct GameStateStruct {
		float					isDayTime;
		bool					isDayTimeChanged;
		float					transitionCurve;
		float					dayLight;
		bool					isExterior;
		bool					isUnderwater;
		bool					isDialog;
		bool					isPersuasion;
		bool					isCellChanged;
		bool					VATSIsOn;
		bool					PipBoyIsOn;
		bool					OverlayIsOn;
		bool					isRainy;
		bool					isSnow;
		bool					isCloudy;
	};

	EffectsStruct			Effects;
	ShadersStruct			Shaders;
	EffectsList				EffectsNames;
	ShaderList				ShaderNames;
	GameStateStruct			GameState;
	ShaderConstants			ShaderConst;
	CustomConstants			CustomConst;
	std::map<std::string, D3DXVECTOR4*>	ConstantsTable;
	IDirect3DVertexBuffer9*	FrameVertex;
	FrameChain				Chain;
	NiD3DVertexShader*		WaterVertexShaders[51];
	NiD3DPixelShader*		WaterPixelShaders[51];
    TESObjectCELL*          PreviousCell;
    bool                    IsMenuSwitch;
    bool                    orthoRequired;
    bool                    avglumaRequired;
	bool					EffectReloadQueued;
	D3DXVECTOR4				SpotLightPosition[SpotLightsMax];
	D3DXVECTOR4				SpotLightColor[SpotLightsMax];
	D3DXVECTOR4				SpotLightDirection[SpotLightsMax];
	D3DXVECTOR4				VolumetricData;		// xyz march light position, w first person strength floor
	D3DXMATRIX				SpotLightWorldToLightMatrix[SpotLightsMax];
	D3DXVECTOR4				LightPosition[TrackedLightsMax];
	D3DXVECTOR4				LightColor[TrackedLightsMax + ShadowCubeMapsMax];
	D3DXVECTOR4				LightAttenuation[TrackedLightsMax];
};

