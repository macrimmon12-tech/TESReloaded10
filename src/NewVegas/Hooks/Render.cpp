#pragma once

#include <cstdarg>
#include <unordered_set>
#include "../../core/GpuProfiler.h"
#include "../../core/GpuTimeline.h"

// Everything from the start of the game's render call up to the world scene: NVR shadow maps,
// the game's own pre-scene work (water reflection/refraction/depth maps and anything else).
static GpuTimer PreSceneTimer("Pre-scene (to world scene)");
static bool PreSceneTimerActive = false;
static void EndPreSceneTimer() {
	if (!PreSceneTimerActive) return;
	PreSceneTimer.End();
	PreSceneTimerActive = false;
}

// ---- F10: the game's own draws split by shader family; first shader uses; shader-bind cost ----
// The world scene, the water reflection map and the first-person model are drawn by the game's render
// loop, most families with NVR's replacement pixel shaders. SetShadersHook sees every geometry pass being
// set up. While F10 profiling is on it puts a GPU timestamp wherever the shader family changes inside one
// of those three passes, and the GpuTimeline adds up the GPU time per family ("GPU SPLIT" lines). It also
// counts binds per frame ("SHADER BINDS" lines) and remembers which D3D pixel shaders have already drawn in
// which pass, so a FRAME SPIKE line can name a shader used for the first time in that frame and the
// longest stall between two binds (the draws of a pass happen between its bind and the next one, so a
// driver compiling a shader at its first draw shows up there).
namespace ShaderSplit {
	enum Context : unsigned char { World, Reflections, FirstPerson, ContextCount, Outside = ContextCount };
	static const char* const ContextNames[ContextCount + 1] = { "world scene", "reflections", "first person", "other" };
	static const unsigned MaxLabels = 32; // label 0: from the start of the pass to its first shader bind
	static_assert(ContextCount * MaxLabels * 2 <= GpuTimeline::KeyCount, "split keys do not fit the timeline");

	static char LabelNames[MaxLabels][16] = { "(pass setup)" };
	static unsigned LabelCount = 1;
	static std::unordered_map<const void*, unsigned char> LabelOfShader;
	static GpuTimeline Timeline("World scene split");

	static unsigned char CurrentContext = Outside;
	static unsigned char CurrentKey = GpuTimeline::NoKey;
	static unsigned char SavedContext[8], SavedKey[8];
	static unsigned Depth = 0;

	// First use of a D3D pixel shader in a pass. Tracked all session, F10 or not, so "first" means first.
	static std::unordered_set<UInt64> Seen;
	static const void* LastHandle = nullptr;
	static unsigned char LastHandleContext = Outside;
	static unsigned FirstUsesSession = 0;

	struct Counters {
		unsigned Binds[ContextCount + 1];			// SetShaders calls (one per geometry pass)
		unsigned PixelChanges[ContextCount + 1];	// ... that changed the pixel shader
		unsigned NvrUploads[ContextCount + 1];		// ... to an NVR shader, so ShaderRecord::SetCT ran
		unsigned FamilyChanges[ContextCount + 1];	// timeline marks
		unsigned FirstUses;
		double HookMs;								// CPU time inside SetShadersHook
	};
	static Counters Frame = {}, Window = {};
	static unsigned WindowFrames = 0;
	static double WindowMaxHookMs = 0.0;
	static bool FrameProfiled = false;

	// This frame's first uses and its longest gap between two binds, for FRAME SPIKE lines.
	struct BindInfo { const char* Shader; unsigned char Context; bool Nvr; bool FirstUse; };
	static BindInfo FirstUseBinds[4];
	static BindInfo LastBind = {}, MaxGapBind = {};
	static double LastBindEnd = 0.0, MaxGapMs = 0.0;

	static unsigned char KeyOf(unsigned context, unsigned label, bool nvr) { return (unsigned char)((context * MaxLabels + label) * 2 + (nvr ? 1 : 0)); }

	// The lit-object pixel shaders (ObjectTemplate.hlsl, src/effects/PBR.h) split by variant group:
	// 2000-2028 sun + up to 3 lights, 2029-2036 up to 6 lights in one pass, 2037-2044 the additive light passes that
	// redraw an object for more lights, 2045-2046 diffuse point lights, 2047-2056 the specular passes.
	static const char* SlsGroup(const char* name) {
		const int number = atoi(name + 3);
		if (number >= 2000 && number <= 2028) return "SLS 1-3 lights";
		if (number >= 2029 && number <= 2036) return "SLS 4+ lights";
		if (number >= 2037 && number <= 2044) return "SLS light pass";
		if (number >= 2045 && number <= 2046) return "SLS diffuse pt";
		if (number >= 2047 && number <= 2056) return "SLS specular";
		return "SLS other";
	}

	// Family of a game pixel shader: the terrain templates by name, the lit-object variant groups (SlsGroup),
	// otherwise the leading letters of the shader name (PAR = parallax objects, SKIN, SM3 = hair and eyes, STLEAF =
	// tree leaves, GRASS, SKY, WATER, ...). Worked out once per shader.
	static unsigned char LabelFor(const NiD3DPixelShader* shader) {
		auto found = LabelOfShader.find(shader);
		if (found != LabelOfShader.end()) return found->second;
		char name[16] = {};
		const char* source = shader ? shader->Name : nullptr;
		const char* terrain = source && TheShaderManager->Shaders.Terrain ? TheShaderManager->Shaders.Terrain->GetTemplate(source).Name : nullptr;
		if (!source) strcpy_s(name, "(no shader)");
		else if (terrain) strcpy_s(name, !strcmp(terrain, "TerrainLODTemplate") ? "TERRAIN LOD" : !strcmp(terrain, "TerrainFadeTemplate") ? "TERRAIN FADE" : "TERRAIN");
		else if (!strncmp(source, "SLS", 3) && source[3] >= '0' && source[3] <= '9') strcpy_s(name, SlsGroup(source));
		else if (!strncmp(source, "SM3", 3)) strcpy_s(name, "SM3");
		else if (!strncmp(source, "SKY", 3)) strcpy_s(name, "SKY");
		else if (!strncmp(source, "WATER", 5)) strcpy_s(name, "WATER");
		else if (!strncmp(source, "ISHDR", 5) || !strncmp(source, "HDR", 3)) strcpy_s(name, "HDR");
		else {
			size_t n = 0;
			while (n < sizeof(name) - 1 && source[n] >= 'A' && source[n] <= 'Z') { name[n] = source[n]; n++; }
			if (!n) strcpy_s(name, "(other)");
		}
		unsigned char label = 0;
		for (unsigned i = 1; i < LabelCount; ++i)
			if (!strcmp(LabelNames[i], name)) { label = (unsigned char)i; break; }
		if (!label) {
			if (LabelCount < MaxLabels - 1) { strcpy_s(LabelNames[LabelCount], name); label = (unsigned char)LabelCount++; }
			else { label = MaxLabels - 1; strcpy_s(LabelNames[label], "(more)"); }
		}
		LabelOfShader[shader] = label;
		return label;
	}

	static void Append(char* buffer, size_t size, size_t& used, const char* format, ...) {
		if (used >= size) return;
		va_list args;
		va_start(args, format);
		const int written = _vsnprintf_s(buffer + used, size - used, _TRUNCATE, format, args);
		va_end(args);
		used = written < 0 ? size : used + written;
	}

	static void SpikeDetail(char* buffer, size_t size) {
		if (!FrameProfiled) return;
		unsigned binds = 0;
		for (unsigned c = 0; c <= ContextCount; ++c) binds += Frame.Binds[c];
		size_t used = 0;
		Append(buffer, size, used, " | shader binds %u (%.2f ms CPU), first uses %u", binds, Frame.HookMs, Frame.FirstUses);
		for (unsigned i = 0; i < Frame.FirstUses && i < 4; ++i)
			Append(buffer, size, used, "%s%s (%s, %s)", i ? ", " : ": ", FirstUseBinds[i].Shader, FirstUseBinds[i].Nvr ? "NVR" : "vanilla", ContextNames[FirstUseBinds[i].Context]);
		if (MaxGapBind.Shader)
			Append(buffer, size, used, " | longest gap between binds %.1f ms, after %s (%s, %s%s)", MaxGapMs, MaxGapBind.Shader,
				MaxGapBind.Nvr ? "NVR" : "vanilla", ContextNames[MaxGapBind.Context], MaxGapBind.FirstUse ? ", first use" : "");
	}

	static void ReportSplit(const GpuTimeline& timeline) {
		struct Row { double Avg, Max; unsigned Label; bool Nvr; };
		for (unsigned c = 0; c < ContextCount; ++c) {
			Row rows[MaxLabels * 2];
			unsigned count = 0;
			double total = 0.0;
			for (unsigned label = 0; label < MaxLabels; ++label) {
				for (unsigned nvr = 0; nvr < 2; ++nvr) {
					const unsigned key = KeyOf(c, label, nvr != 0);
					const double avg = timeline.AverageMs(key);
					if (avg <= 0.0) continue;
					rows[count++] = { avg, timeline.MaxMs(key), label, nvr != 0 };
					total += avg;
				}
			}
			if (!count) continue;
			std::sort(rows, rows + count, [](const Row& a, const Row& b) { return a.Avg > b.Avg; });
			Logger::Log("GPU SPLIT %s, %u frames: %.4f ms per frame by shader family (NVR = NVR's replacement shader, vanilla = the game's own)",
				ContextNames[c], timeline.WindowFrames(), total);
			for (unsigned i = 0; i < count; ++i)
				Logger::Log("GPU SPLIT   %-12s %-14s %-7s avg %.4f ms  max %.4f  (%4.1f%%)", ContextNames[c], LabelNames[rows[i].Label],
					rows[i].Nvr ? "NVR" : "vanilla", rows[i].Avg, rows[i].Max, 100.0 * rows[i].Avg / total);
		}
		Logger::Log("GPU SPLIT   most family changes in one frame %u; frames dropped (over %u changes) %u, rejected (clock) %u",
			timeline.MostMarks(), GpuTimeline::MaxMarks, timeline.DroppedFrames(), timeline.RejectedFrames());
	}

	static void ReportCounters() {
		const double n = WindowFrames;
		char line[768] = {};
		size_t used = 0;
		Append(line, sizeof(line), used, "SHADER BINDS per frame (%u frames): binds / pixel shader changes / of those to NVR shaders / family changes:", WindowFrames);
		for (unsigned c = 0; c <= ContextCount; ++c)
			Append(line, sizeof(line), used, "%s %s %.0f / %.0f / %.0f / %.0f", c ? " |" : "", ContextNames[c], Window.Binds[c] / n, Window.PixelChanges[c] / n,
				Window.NvrUploads[c] / n, Window.FamilyChanges[c] / n);
		Logger::Log("%s", line);
		Logger::Log("SHADER BINDS   CPU inside SetShaders avg %.3f ms max %.3f per frame | first shader uses %u in these frames, %u this session",
			Window.HookMs / n, WindowMaxHookMs, Window.FirstUses, FirstUsesSession);
	}

	static void BeginFrame(IDirect3DDevice9* device) {
		static bool connected = false;
		if (!connected) {
			FrameTimeMonitor::SpikeDetail = &SpikeDetail;
			Timeline.OnReport = &ReportSplit;
			connected = true;
		}
		Depth = 0;
		CurrentContext = Outside;
		CurrentKey = GpuTimeline::NoKey;
		Frame = {};
		LastBind = { "(frame start)", Outside, false, false };
		MaxGapBind = {};
		MaxGapMs = 0.0;
		FrameProfiled = GpuTimer::Enabled && !InterfaceManager->IsActive(Menu::kMenuType_Loading);
		LastBindEnd = FrameProfiled ? CpuTimer::NowMs() : 0.0;
		if (FrameProfiled) Timeline.BeginFrame(device);
	}

	static void EndFrame() {
		Timeline.EndFrame();
		if (!FrameProfiled || !GpuTimer::Enabled) return;
		for (unsigned c = 0; c <= ContextCount; ++c) {
			Window.Binds[c] += Frame.Binds[c];
			Window.PixelChanges[c] += Frame.PixelChanges[c];
			Window.NvrUploads[c] += Frame.NvrUploads[c];
			Window.FamilyChanges[c] += Frame.FamilyChanges[c];
		}
		Window.FirstUses += Frame.FirstUses;
		Window.HookMs += Frame.HookMs;
		if (Frame.HookMs > WindowMaxHookMs) WindowMaxHookMs = Frame.HookMs;
		if (++WindowFrames >= 120) {
			ReportCounters();
			Window = {};
			WindowFrames = 0;
			WindowMaxHookMs = 0.0;
		}
	}

	// The game pass being drawn (nests: the stack restores the outer pass and its current family).
	static void BeginContext(Context context) {
		if (Depth < 8) { SavedContext[Depth] = CurrentContext; SavedKey[Depth] = CurrentKey; }
		Depth++;
		CurrentContext = context;
		CurrentKey = Timeline.InFrame() ? KeyOf(context, 0, false) : GpuTimeline::NoKey;
		Timeline.Mark(CurrentKey);
	}

	static void EndContext() {
		if (!Depth) return;
		Depth--;
		CurrentContext = Depth < 8 ? SavedContext[Depth] : (unsigned char)Outside;
		CurrentKey = Depth < 8 ? SavedKey[Depth] : GpuTimeline::NoKey;
		Timeline.Mark(CurrentKey);
	}

	// Called from SetShadersHook once the pixel shader for the pass is chosen. Start: when the hook began.
	static void OnBind(const NiD3DPixelShader* shader, const IDirect3DPixelShader9* previous, double start) {
		const unsigned char context = CurrentContext;
		const IDirect3DPixelShader9* handle = shader ? shader->ShaderHandle : nullptr;
		const bool nvr = handle && handle != (const IDirect3DPixelShader9*)shader->ShaderHandleBackup;
		bool firstUse = false;
		if (handle && (handle != LastHandle || context != LastHandleContext)) {
			LastHandle = handle;
			LastHandleContext = context;
			firstUse = Seen.insert(((UInt64)(uintptr_t)handle << 2) | (context & 3)).second;
			if (firstUse) FirstUsesSession++;
		}
		if (!FrameProfiled) return;

		// The previous pass drew between the end of its bind and the start of this one.
		if (LastBindEnd > 0.0 && start - LastBindEnd > MaxGapMs) { MaxGapMs = start - LastBindEnd; MaxGapBind = LastBind; }
		Frame.Binds[context]++;
		if (handle != previous) {
			Frame.PixelChanges[context]++;
			if (nvr) Frame.NvrUploads[context]++;
		}
		LastBind = { shader ? shader->Name : "(no pixel shader)", context, nvr, firstUse };
		if (firstUse && Frame.FirstUses++ < 4) FirstUseBinds[Frame.FirstUses - 1] = LastBind;

		if (context == Outside || !Timeline.InFrame()) return;
		const unsigned char key = KeyOf(context, LabelFor(shader), nvr);
		if (key != CurrentKey) {
			Timeline.Mark(key);
			CurrentKey = key;
			Frame.FamilyChanges[context]++;
		}
	}

	static void EndBind(double start) {
		const double now = CpuTimer::NowMs();
		Frame.HookMs += now - start;
		LastBindEnd = now;
	}
}

// ---- Which of the game's render entry points ran this frame ----
// The depth buffers NVR's effects read are refreshed only inside RenderWorldSceneGraphHook. After a save is
// loaded straight into an interior the game has been seen to render frames without entering it at all
// (the F10 log then shows no 'World scene (game)' / 'Depth resolves' samples and 'Pre-scene' equal to the
// whole frame), and the effects run on a stale depth buffer: the scene comes out almost black. This
// records what happened on the first frames after each cell change, and lets ProcessImageSpaceShadersHook
// skip NVR's effects while the world scene keeps going missing (the WorldSceneGuard switch).
static bool WorldRenderedThisFrame = false;
static unsigned WorldMissStreak = 0;        // consecutive earlier frames without a world scene render
static unsigned FrameWorldCalls = 0, FrameFirstPersonCalls = 0, FrameImageSpaceCalls = 0;
static bool FrameWorldArgsKnown = false;
static int FrameWorldArgs[3] = {};          // IsFirstPerson, WireFrame, Arg4 of the first call this frame
static const unsigned WorldGuardFrames = 10;

// The world is not drawn behind the main menu or a loading screen, so those frames say nothing about it.
static bool WorldRenderExpected() {
	return Player && Player->parentCell && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main) &&
		!InterfaceManager->IsActive(Menu::MenuType::kMenuType_Loading);
}

static bool WorldSceneGuardActive() {
	return !TheSettingManager->SettingsMain.Main.DisableWorldSceneGuard && !WorldRenderedThisFrame &&
		WorldMissStreak >= WorldGuardFrames && WorldRenderExpected();
}

static void ReportWorldRender(BSRenderedTexture* RenderedTexture, int Arg2, int Arg3) {
	static TESObjectCELL* lastCell = nullptr;
	static unsigned frame = 0, logged = 0, tracePending = 0;
	static bool guardLogged = false;
	frame++;

	TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
	if (!cell) { WorldMissStreak = 0; lastCell = nullptr; return; } // no cell: nothing to report
	if (!WorldRenderExpected()) { WorldMissStreak = 0; return; }     // main menu / loading screen
	if (cell != lastCell) { lastCell = cell; tracePending = 3; }

	const bool missed = !WorldRenderedThisFrame;
	const bool started = missed && WorldMissStreak == 0;
	const bool recovered = !missed && WorldMissStreak >= 3;
	const bool guarded = missed && WorldMissStreak >= WorldGuardFrames;

	if (logged < 80 && (tracePending || started || recovered || (guarded && !guardLogged))) {
		const char* name = cell->GetEditorName();
		Logger::Log("WORLD TRACE frame %u: cell %08X '%s' interior=%d behaveLikeExterior=%d | world scene calls %u%s | first person %u, image space %u | "
			"render args rt=%p %d %d | menuBackgroundReady=%d mainMenu=%d loading=%d | miss streak %u%s%s",
			frame, cell->refID, name ? name : "?", cell->IsInterior() ? 1 : 0, (cell->flags0 & TESObjectCELL::kFlags0_BehaveLikeExterior) ? 1 : 0,
			FrameWorldCalls, FrameWorldArgsKnown ? (FrameWorldArgs[0] ? " (first-person pass)" : " (world pass)") : " (none)",
			FrameFirstPersonCalls, FrameImageSpaceCalls, RenderedTexture, Arg2, Arg3,
			TESMain::IsMenuBackgroundReady() ? 1 : 0, InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main) ? 1 : 0,
			InterfaceManager->IsActive(Menu::MenuType::kMenuType_Loading) ? 1 : 0, WorldMissStreak,
			started ? " [WORLD SCENE NOT RENDERED]" : "", recovered ? " [world scene rendering again]" : "");
		logged++;
		if (tracePending) tracePending--;
		if (guarded) guardLogged = true;
	}
	if (recovered) guardLogged = false;

	WorldMissStreak = missed ? WorldMissStreak + 1 : 0;
}

void (__thiscall* Render)(Main*, BSRenderedTexture*, int, int) = (void (__thiscall*)(Main*, BSRenderedTexture*, int, int))Hooks::Render;
void __fastcall RenderHook(Main* This, UInt32 edx, BSRenderedTexture* RenderedTexture, int Arg2, int Arg3) {
	
	SettingsMainStruct* SettingsMain = &TheSettingManager->SettingsMain;

	TheFrameRateManager->UpdatePerformance();
	TheCameraManager->SetSceneGraph();
	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();

	TheShaderManager->UpdateConstants();

	// Reset the material pass queue for the frame. Passing false when the flashlight is
	// not lit leaves it inactive, so the scene walk in RenderWorldSceneGraphHook costs
	// nothing while the light is off.
	FlashlightEffect* Flashlight = TheShaderManager->Effects.Flashlight;
	MaterialPass::BeginFrame(Flashlight->Enabled && Flashlight->spotLightActive);

	//if (SettingsMain->Develop.TraceShaders && InterfaceManager->IsActive(Menu::MenuType::kMenuType_None) && Global->OnKeyDown(SettingsMain->Develop.TraceShaders) && DWNode::Get() == NULL) DWNode::Create();
	// Whole game frame on the GPU (scene, reflections, NVR effects, image space). Comparing it
	// with the individual buckets shows how much GPU time is not attributed to any of them.
	static GpuTimer frameTimer("Game frame total");
	GpuProfileScope gpu(frameTimer, TheRenderManager->device);
	PreSceneTimerActive = PreSceneTimer.Begin(TheRenderManager->device);
	WorldRenderedThisFrame = false;
	FrameWorldCalls = FrameFirstPersonCalls = FrameImageSpaceCalls = 0;
	FrameWorldArgsKnown = false;
	ShaderSplit::BeginFrame(TheRenderManager->device);
	(*Render)(This, RenderedTexture, Arg2, Arg3);
	ShaderSplit::EndFrame();
	EndPreSceneTimer();
	ReportWorldRender(RenderedTexture, Arg2, Arg3);

}

// Set when NVR changes, mid-frame, a TESR_ constant that game shaders read (CheapReflections). Constants
// reach a game shader only in ShaderRecord::SetCT, which runs when the pixel shader CHANGES, so the next
// bind is told that no pixel shader is bound: an NVR shader then uploads even if the game keeps the one
// already bound. Nothing else changes (the game still sets the device shader itself).
static bool ForcePixelConstants = false;

// ReducedQuality CheapReflections: the water reflection map is drawn without the forward sun-shadow
// lookup (TESR_ShadowForwardData.x = 1, the forward path's own off switch) and without terrain parallax
// (TESR_TerrainParallaxData.x = 0), like the game's own reflections. Both are restored, and uploaded
// again on the next bind, when the pass ends.
class CheapReflectionScope {
public:
	CheapReflectionScope() {
		if (!TheSettingManager->SettingsMain.Main.CheapReflections) return;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		TerrainShaders* terrain = TheShaderManager->Shaders.Terrain;
		if (!shadows || !terrain) return;
		Forward = &shadows->Constants.ForwardData.x;
		Parallax = &terrain->ParallaxConstants.Data.x;
		SavedForward = *Forward;
		SavedParallax = *Parallax;
		*Forward = 1.0f;
		*Parallax = 0.0f;
		ForcePixelConstants = true;
		static bool announced = false;
		if (!announced) {
			Logger::Log("Cheap reflections: water reflection map drawn without sun shadows and terrain parallax.");
			announced = true;
		}
	}
	~CheapReflectionScope() {
		if (!Forward) return;
		*Forward = SavedForward;
		*Parallax = SavedParallax;
		ForcePixelConstants = true;
	}

private:
	float* Forward = nullptr;
	float* Parallax = nullptr;
	float SavedForward = 0.0f;
	float SavedParallax = 0.0f;
};

void (__thiscall* SetShaders)(BSShader*, UInt32) = (void (__thiscall*)(BSShader*, UInt32))Hooks::SetShaders;
void __fastcall SetShadersHook(BSShader* This, UInt32 edx, UInt32 PassIndex) {
	
	const bool profiling = ShaderSplit::FrameProfiled;
	const double bindStart = profiling ? CpuTimer::NowMs() : 0.0;
	NiGeometry* Geometry = *(NiGeometry**)(*(void**)0x011F91E0);
	NiD3DPass* Pass = *(NiD3DPass**)0x0126F74C;
	NiD3DVertexShaderEx* VertexShader = (NiD3DVertexShaderEx*)Pass->VertexShader;
	NiD3DPixelShaderEx* PixelShader = (NiD3DPixelShaderEx*)Pass->PixelShader;
	IDirect3DVertexShader9* VertexShader2 = TheRenderManager->renderState->GetVertexShader();
	IDirect3DPixelShader9* PixelShader2 = TheRenderManager->renderState->GetPixelShader();

	if (VertexShader) {
		VertexShader->SetupShader(VertexShader2);
	}
	else {
		Logger::Log("Error getting vertex shader for pass %s", Pointers::Functions::GetPassDescription(PassIndex));
	}
	if (PixelShader) {
		PixelShader->SetupShader(ForcePixelConstants ? nullptr : PixelShader2);
		ForcePixelConstants = false;
	}
	else {
		Logger::Log("Error getting pixel shader for pass %s", Pointers::Functions::GetPassDescription(PassIndex));
	}
	ShaderSplit::OnBind(PixelShader, PixelShader2, bindStart);

	// trace pipeline active shaders
	if (TheSettingManager->SettingsMain.Develop.DebugMode && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_Console) && Global->OnKeyDown(TheSettingManager->SettingsMain.Develop.TraceShaders)) {
		char Name[256];
		sprintf(Name, "Pass %i %s, %s (%s %s)", PassIndex, Pointers::Functions::GetPassDescription(PassIndex), Geometry->m_pcName, VertexShader->Name, PixelShader->Name);
		if (VertexShader->ShaderHandle == VertexShader->ShaderHandleBackup) strcat(Name, " - Vertex: vanilla");
		if (PixelShader->ShaderHandle == PixelShader->ShaderHandleBackup) strcat(Name, " - Pixel: vanilla");
		Logger::Log("%s", Name);
		InterfaceManager->ShowMessage("Shaders Traced");
		//DWNode::AddNode(Name, Geometry->m_parent, Geometry);
	}
	(*SetShaders)(This, PassIndex);
	if (profiling) ShaderSplit::EndBind(bindStart);

}

HRESULT (__thiscall* SetSamplerState)(NiDX9RenderState*, UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8) = (HRESULT (__thiscall*)(NiDX9RenderState*, UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8))Hooks::SetSamplerState;
HRESULT __fastcall SetSamplerStateHook(NiDX9RenderState* This, UInt32 edx, UInt32 Sampler, D3DSAMPLERSTATETYPE Type, UInt32 Value, UInt8 Save) {

	UInt16* TypeMap = (UInt16*)0x126F92C;
	HRESULT r = D3D_OK;

	if (TypeMap[Type] < 5)
		r = (*SetSamplerState)(This, Sampler, Type, Value, Save);
	else
		r = TheRenderManager->device->SetSamplerState(Sampler, Type, Value);
	return r;

}

void (__thiscall* RenderWorldSceneGraph)(Main*, Sun*, UInt8, UInt8, UInt8) = (void (__thiscall*)(Main*, Sun*, UInt8, UInt8, UInt8))Hooks::RenderWorldSceneGraph;
void __fastcall RenderWorldSceneGraphHook(Main* This, UInt32 edx, Sun* SkySun, UInt8 IsFirstPerson, UInt8 WireFrame, UInt8 Arg4) {
	// TAA sub-pixel jitter covers the world scene and nothing else -- see TAAEffect::BeginJitter.
	TAAEffect* TAA = TheShaderManager->Effects.TAA;
	if (TAA) TAA->BeginJitter();
	EndPreSceneTimer();
	WorldRenderedThisFrame = true;
	if (!FrameWorldArgsKnown) { FrameWorldArgsKnown = true; FrameWorldArgs[0] = IsFirstPerson; FrameWorldArgs[1] = WireFrame; FrameWorldArgs[2] = Arg4; }
	FrameWorldCalls++;
	{
		// Game geometry drawn with NVR's replacement shaders, including per-object sun shadows.
		static GpuTimer worldTimer("World scene (game)");
		GpuProfileScope gpu(worldTimer, TheRenderManager->device);
		ShaderSplit::BeginContext(ShaderSplit::World);
		(*RenderWorldSceneGraph)(This, SkySun, IsFirstPerson, WireFrame, Arg4);
		ShaderSplit::EndContext();
	}

	// Re-light nearby statics inside the flashlight cone. This has to happen here, before
	// the viewmodel depth handling below clears the Z buffer: the pass draws with depth
	// testing on and depth writes off, so with a cleared Z buffer it would draw straight
	// through world geometry.
	MaterialPass::CaptureScene(WorldSceneGraph);
	MaterialPass::RenderWorld();

	// After the material pass, not before: it redraws world geometry over the scene, and must
	// land on exactly the same jittered pixels the world did.
	if (TAA) TAA->EndJitter();

	const bool bPipBoyOpen = InterfaceManager->IsPipBoyOpen();
	const bool bPipBoyLive = (TheGameMenuManager->IsLiveMenu && TheGameMenuManager->IsLiveMenu(Menu::kMenuType_BigFour, false, false) == GameMenuManager::MenuPauseState::MENU_LIVE);

	static GpuTimer depthResolveTimer("Depth resolves");
	GpuProfileScope gpuResolve(depthResolveTimer, TheRenderManager->device);
	if (!bPipBoyOpen || bPipBoyLive)
		TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture); // disable updating the world buffer when pipboy is out

	if (!IsFirstPerson) {
		// clear the viewmodel depth buffer
		TheRenderManager->Clear(NULL, NiRenderer::kClear_ZBUFFER);
		TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTextureViewModel);
	}
}

void (__thiscall* RenderFirstPerson)(Main*, NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*) = (void (__thiscall*)(Main*, NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*))Hooks::RenderFirstPerson;
void __fastcall RenderFirstPersonHook(Main* This, UInt32 edx, NiDX9Renderer* Renderer, NiGeometry* Geo, Sun* SkySun, BSRenderedTexture* RenderedTexture) {
	// Clear the depth buffer before rendering first person model to prevent clipping with world objects & other artefacts
	static GpuTimer firstPersonTimer("First person (game)");
	GpuProfileScope gpu(firstPersonTimer, TheRenderManager->device);
	FrameFirstPersonCalls++;
	TheRenderManager->Clear(NULL, NiRenderer::kClear_ZBUFFER);
	//ThisCall(0x00874C10, Global);
	ShaderSplit::BeginContext(ShaderSplit::FirstPerson);
	(*RenderFirstPerson)(This, Renderer, Geo, SkySun, RenderedTexture);
	ShaderSplit::EndContext();
	TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTextureViewModel);
}

void (__thiscall* RenderReflections)(WaterManager*, NiCamera*, ShadowSceneNode*) = (void (__thiscall*)(WaterManager*, NiCamera*, ShadowSceneNode*))Hooks::RenderReflections;
void __fastcall RenderReflectionsHook(WaterManager* This, UInt32 edx, NiCamera* Camera, ShadowSceneNode* SceneNode) {
	if (!TheSettingManager->SettingsMain.Main.ForceReflections) {
		// Hooked for profiling in this mode: the game's reflection pass, unchanged unless CheapReflections is on.
		static GpuTimer reflectionsTimer("Water reflections (game)");
		static CpuTimer reflectionsCpuTimer("Water reflections (CPU)");
		CpuProfileScope cpu(reflectionsCpuTimer);
		GpuProfileScope gpu(reflectionsTimer, TheRenderManager->device);
		CheapReflectionScope cheap;
		ShaderSplit::BeginContext(ShaderSplit::Reflections);
		(*RenderReflections)(This, Camera, SceneNode);
		ShaderSplit::EndContext();
		return;
	}
	
	D3DXVECTOR4* ShadowData = &TheShaderManager->Effects.ShadowsExteriors->Constants.Data;
	float ShadowDataBackup = ShadowData->x;

	D3DXVECTOR4* TerrainParallaxData = &TheShaderManager->Shaders.Terrain->ParallaxConstants.Data;
	float TerrainParallaxBackup = TerrainParallaxData->x;

	if (DWNode::Get()) DWNode::AddNode("BEGIN REFLECTIONS RENDERING", NULL, NULL);
	ShadowData->x = -1.0f; // Disables the shadows rendering for water reflections (the geo is rendered with the same shaders used in the normal scene!)
	TerrainParallaxData->x = 0;
	{
		// ForceReflections renders the full world again into the water reflection map.
		static GpuTimer reflectionsTimer("Water reflections (game)");
		static CpuTimer reflectionsCpuTimer("Water reflections (CPU)");
		CpuProfileScope cpu(reflectionsCpuTimer);
		GpuProfileScope gpu(reflectionsTimer, TheRenderManager->device);
		CheapReflectionScope cheap;
		ShaderSplit::BeginContext(ShaderSplit::Reflections);
		(*RenderReflections)(This, Camera, SceneNode);
		ShaderSplit::EndContext();
	}
	ShadowData->x = ShadowDataBackup;
	TerrainParallaxData->x = TerrainParallaxBackup;
	if (DWNode::Get()) DWNode::AddNode("END REFLECTIONS RENDERING", NULL, NULL);
}

void (__thiscall* RenderPipboy)(Main*, NiGeometry*, NiDX9Renderer*) = (void (__thiscall*)(Main*, NiGeometry*, NiDX9Renderer*))Hooks::RenderPipboy;
void __fastcall RenderPipboyHook(Main* This, UInt32 edx, NiGeometry* Geo, NiDX9Renderer* Renderer) {
	WorldSceneGraph->UpdateParticleShaderFoV(Player->firstPersonFoV);
//	Player->SetFoV(Player->firstPersonFoV);
	(*RenderPipboy)(This, Geo, Renderer);
}

float (__thiscall* GetWaterHeightLOD)(TESWorldSpace*) = (float (__thiscall*)(TESWorldSpace*))Hooks::GetWaterHeightLOD;
float __fastcall GetWaterHeightLODHook(TESWorldSpace* This, UInt32 edx) {
	
	float r = This->waterHeight;
	if (*(void**)This == (void*)0x0103195C) r = TheShaderManager->Shaders.Water->Constants.Default.waterSettings.x;
	return r;

}

bool bSkippedRender_RenderedMenu = false;
bool bDoneRender_LockPickMenu = false;

void(__cdecl* ProcessImageSpaceShaders)(NiDX9Renderer*, BSRenderedTexture*, BSRenderedTexture*) = (void(__cdecl*)(NiDX9Renderer*, BSRenderedTexture*, BSRenderedTexture*))Hooks::ProcessImageSpaceShaders;
void __cdecl ProcessImageSpaceShadersHook(NiDX9Renderer* Renderer, BSRenderedTexture* SourceTarget, BSRenderedTexture* DestinationTarget) {
	bool bLiveRenderedMenu = false; // FORenderedMenu, FOPipBoyManager
	bool bLive3DMenu = false; // Normal menus, but 3D, lockpick etc
	if (TESMain::IsMenuBackgroundReady() && TheGameMenuManager->IsLiveMenu && InterfaceManager->currentMode != 1) {
		const bool bLockPickMenu = LockPickMenu::GetSingleton() && TheGameMenuManager->IsLiveMenu(Menu::kMenuType_LockPick, false, false) == GameMenuManager::MENU_LIVE;
		const bool bPipBoyLive = InterfaceManager->IsPipBoyOpen() && TheGameMenuManager->IsLiveMenu(Menu::kMenuType_BigFour, false, false) == GameMenuManager::MenuPauseState::MENU_LIVE;
		const bool bRenderedMenuLive = InterfaceManager->pRenderedMenu && TheGameMenuManager->IsLiveMenu(InterfaceManager->menuStack[0], false, false) == GameMenuManager::MenuPauseState::MENU_LIVE;
		bLiveRenderedMenu = bPipBoyLive || bRenderedMenuLive;
		bLive3DMenu = bLockPickMenu;
	}

	if (bLive3DMenu) {
		if (bDoneRender_LockPickMenu) {
			bDoneRender_LockPickMenu = false;
			ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
			return;
		}
		else {
			bDoneRender_LockPickMenu = true;
		}
	}
	else {
		bDoneRender_LockPickMenu = false;
	}
	
	if (bLiveRenderedMenu) {
		if (!bSkippedRender_RenderedMenu) {
			bSkippedRender_RenderedMenu = true;
			ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
			return;
		}
		else {
			bSkippedRender_RenderedMenu = false;
		}
	}
	else {
		bSkippedRender_RenderedMenu = false;
	}

	FrameImageSpaceCalls++;
	if (WorldSceneGuardActive()) {
		// No world scene has been rendered for several frames, so the depth buffers NVR's effects need are
		// stale and they would darken the image. Let the game's own image space run on its own.
		static bool announced = false;
		if (!announced) { Logger::Log("World scene guard: no world scene render for %u frames, NVR effects skipped until it returns.", WorldMissStreak); announced = true; }
		ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
		return;
	}

	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	IDirect3DSurface9* GameSurface = NULL;
	IDirect3DSurface9* OutputSurface = NULL;
	
	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();
	TheShaderManager->UpdateConstants();

	if (SourceTarget && TheSettingManager->SettingsMain.Main.RenderPreTonemapping) {
		SourceTarget->GetD3DTexture(0)->GetSurfaceLevel(0, &GameSurface); // get the surface from the game render target

		// Disable render state settings that create artefacts
		RenderState->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ZWRITEENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILMASK, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILWRITEMASK, 255, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILREF, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_COLORWRITEENABLE, 15, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ALPHATESTENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ALPHABLENDENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ALPHAREF, 0, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_NORMALIZENORMALS, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_POINTSIZE, 810365505, RenderStateArgs); // fix flickering linked to alpha somehow

		TheShaderManager->RenderEffectsPreTonemapping(GameSurface);
	
	}

	{
		static GpuTimer imageSpaceTimer("Game image space");
		GpuProfileScope gpu(imageSpaceTimer, Device);
		ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
	}

	if (!DestinationTarget && TheRenderManager->currentRTGroup) {
		OutputSurface = TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface;
		if (!TheSettingManager->SettingsMain.Main.RenderPreTonemapping) TheShaderManager->RenderEffectsPreTonemapping(OutputSurface);
		TheShaderManager->RenderEffects(OutputSurface);
		TheRenderManager->CheckAndTakeScreenShot(OutputSurface, TheSettingManager->SettingsMain.Main.HDRScreenshot);
	}

	if (GameSurface) GameSurface->Release();
}

static void RenderMainMenuMovie() {

	if (TheSettingManager->SettingsMain.Main.ReplaceIntro && InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main))
		TheBinkManager->Render(MainMenuMovie);
	else
		TheBinkManager->Close();

}

CallDetour kRenderInterfaceDetour;
void __fastcall RenderInterfaceHook(void* apThis, void*, void* apCuller, bool abPipboyVisible) {
	RenderMainMenuMovie();
	ImGuiManager::NewFrame();
	ThisCall(kRenderInterfaceDetour.GetOverwrittenAddr(), apThis, apCuller, abPipboyVisible);
	ImGuiManager::Render();
}

static void SetTileShaderConstants() {
	
	float ViewProj[16];
	NiVector4 TintColor = { 1.0f, 1.0f, 1.0f, 0.0f };

	if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main)) {
		TheRenderManager->device->GetVertexShaderConstantF(0, ViewProj, 4);
		if ((int)ViewProj[3] == -1 && (int)ViewProj[7] == 1 && (int)ViewProj[15] == 1) TheRenderManager->device->SetPixelShaderConstantF(0, (const float*)&TintColor, 1);
	}

}

__declspec(naked) void SetTileShaderConstantsHook() {

	__asm {
		pushad
		call	SetTileShaderConstants
		popad
		cmp		byte ptr [esi + 0xAC], 0
		jmp		Jumpers::SetTileShaderConstants::Return
	}

}

void* (__thiscall* ShowDetectorWindow)(DetectorWindow*, HWND, HINSTANCE, NiNode*, char*, int, int, int, int) = (void* (__thiscall*)(DetectorWindow*, HWND, HINSTANCE, NiNode*, char*, int, int, int, int))::Hooks::ShowDetectorWindow;
void* __fastcall ShowDetectorWindowHook(DetectorWindow* This, UInt32 edx, HWND Handle, HINSTANCE Instance, NiNode* RootNode, char* FormCaption, int X, int Y, int Width, int Height) {
	
	NiAVObject* Object = NULL;
	void* r = NULL;

	r = (ShowDetectorWindow)(This, Handle, Instance, RootNode, (char*)"Pipeline detector by Alenet", X, Y, 1280, 1024);
	for (int i = 0; i < RootNode->m_children.end; i++) {
		NiNode* Node = (NiNode*)RootNode->m_children.data[i];
		Node->m_children.data[0] = NULL;
		Node->m_children.data[1] = NULL;
		Node->m_children.end = 0;
		Node->m_children.numObjs = 0;
	}
	return r;

}

void DetectorWindowSetNodeName(char* Buffer, int Size, char* Format, char* ClassName, char* Name, float LPosX, float LPosY, float LPosZ) {

	sprintf(Buffer, "%s", Name);

}

static void DetectorWindowCreateTreeView(HWND TreeView) {

	HFONT Font = CreateFontA(14, 0, 0, 0, FW_DONTCARE, NULL, NULL, NULL, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
	SendMessageA(TreeView, WM_SETFONT, (WPARAM)Font, TRUE);
	SendMessageA(TreeView, TVM_SETBKCOLOR, NULL, 0x001E1E1E);
	SendMessageA(TreeView, TVM_SETTEXTCOLOR, NULL, 0x00DCDCDC);

}

__declspec(naked) void DetectorWindowCreateTreeViewHook() {

	__asm {
		pushad
		push	eax
		call	DetectorWindowCreateTreeView
		pop		eax
		popad
		mov     ecx, [ebp - 0x48]
		mov		[ecx + 0x0C], eax
		mov     esp, ebp
		pop     ebp
		jmp		Jumpers::DetectorWindow::CreateTreeViewReturn
	}

}

void DetectorWindowDumpAttributes(HWND TreeView, UInt32 Msg, WPARAM wParam, LPTVINSERTSTRUCTA lParam) {

	TVITEMEXA Item = { NULL };
	char T[260] = { '\0' };

	Item.pszText = T;
	Item.mask = TVIF_TEXT;
	Item.hItem = (HTREEITEM)SendMessageA(TreeView, TVM_GETNEXTITEM, TVGN_PARENT, (LPARAM)lParam->hParent);
	Item.cchTextMax = 260;
	SendMessageA(TreeView, TVM_GETITEMA, 0, (LPARAM)&Item);
	if (!memcmp(Item.pszText, "Pass", 4))
		SendMessageA(TreeView, TVM_DELETEITEM, 0, (LPARAM)lParam->hParent);
	else
		if (strlen(Item.pszText)) SendMessageA(TreeView, Msg, wParam, (LPARAM)lParam);

}

__declspec(naked) void DetectorWindowDumpAttributesHook() {

	__asm {
		call	DetectorWindowDumpAttributes
		add		esp, 16
		jmp		Jumpers::DetectorWindow::DumpAttributesReturn
	}

}

__declspec(naked) void DetectorWindowConsoleCommandHook() {

	__asm {
		call	DWNode::Create
		jmp		Jumpers::DetectorWindow::ConsoleCommandReturn
	}

}

// Enables culling of muzzle flashes so they don't stay after firing
void __fastcall MuzzleLightCullingFix(MuzzleFlash* This) {
	if (This->light) {
		if (!This->bEnabled) {
			This->light->m_flags |= 1;
		}
		else {
			This->light->m_flags &= ~1;
		}
	}
	ThisCall(0x9BB8A0, This);
}

BSFogProperty* __cdecl ShadowSceneNode__GetFogPropertyEx(UInt32 aeType) {
	if (TheShaderManager->Effects.VolumetricFog->Enabled && !TheShaderManager->GameState.isUnderwater) {
		aeType = 1;  // Use UI scene node and thus render no fog.
	}

	return BSShaderManager::GetShadowSceneNode(static_cast<BSShaderManager::SceneGraphType>(aeType))->fogProperty;
}

NiPoint2* __fastcall WaterFogRemover(NiPoint2* point, void*, float x, float y)
{
	point->x = 0.f;
	point->y = 0.f;
	return point;
}

// Compatibility patch for DXVK 16bits buffer upgrade.
typedef bool(__cdecl* DisableFormatUpgradeFunc)();
typedef bool(__cdecl* EnableFormatUpgradeFunc)();

BSRenderedTexture* (__cdecl* CreateBSRenderedTexture)(BSString*, const UInt32, const UInt32, NiTexture::FormatPrefs*, UInt32, bool, NiDepthStencilBuffer*, UInt32, UInt32) = (BSRenderedTexture * (__cdecl*)(BSString*, const UInt32, const UInt32, NiTexture::FormatPrefs*, UInt32, bool, NiDepthStencilBuffer*, UInt32, UInt32))Hooks::CreateRenderedTexture;
BSRenderedTexture* __cdecl CreateSaveTextureHook(BSString* apName, const UInt32 uiWidth, const UInt32 uiHeight, NiTexture::FormatPrefs* kPrefs, 
	UInt32 eMSAAPref, bool bUseDepthStencil, NiDepthStencilBuffer* pkDSBuffer, UInt32 a7, UInt32 uiBackgroundColor) {
	HMODULE hDLL = GetModuleHandle(L"d3d9.dll");

	// If the loaded library is DXVK-HDR (https://github.com/EndlesslyFlowering/dxvk), these will pass
	DisableFormatUpgradeFunc disable = (DisableFormatUpgradeFunc)GetProcAddress(hDLL, "DXVK_D3D9_HDR_DisableRenderTargetUpgrade");
	EnableFormatUpgradeFunc enable = (DisableFormatUpgradeFunc)GetProcAddress(hDLL, "DXVK_D3D9_HDR_EnableRenderTargetUpgrade");

	if (disable)
		disable(); // Temporarily disable the format upgrade for the texture
	BSRenderedTexture* pTexture = CreateBSRenderedTexture(apName, uiWidth, uiHeight, kPrefs, eMSAAPref, bUseDepthStencil, pkDSBuffer, a7, uiBackgroundColor);
	if (enable)
		enable(); // Restore the format upgrade functionality 

	return pTexture;
}

// 0xE69660
bool __fastcall NiDX9Renderer__Do_EndFrame(NiDX9Renderer* apThis, void*) {
	bool bResult = ThisStdCall<bool>(0xE69660, apThis);

	// Reload effects if queued.
	if (TheShaderManager && TheShaderManager->EffectReloadQueued) {
		TheShaderManager->ReloadEffects();
		TheShaderManager->EffectReloadQueued = false;
	}

	return bResult;
}


// Code to increase all lights strength
//__forceinline NiVector4* GetConstant(int index) {
//	return &((NiVector4*)0x11FA0C0)[index];
//}
//
//__forceinline NiColorAlpha* GetLightConstant(int index) {
//	return reinterpret_cast<NiColorAlpha*>(GetConstant(index));
//}
//
//__forceinline void ScaleColor(NiColorAlpha* Color, float scale) {
//	Color->r *= scale;
//	Color->g *= scale;
//	Color->b *= scale;
//}
//
//void __fastcall ShadowLightShader__UpdateLights(void* apThis, void*, void* apShaderProp, void* apRenderPass, D3DXMATRIX aMatrix, void* apTransform, UInt32 aeRenderPassType, void* apSkinInstance) {
//	ThisCall(0xB78A90, apThis, apShaderProp, apRenderPass, aMatrix, apTransform, aeRenderPassType, apSkinInstance);
	//Logger::Log("scaling light by %f", TheShaderManager->ShaderConst.HDR.PointLightMult);
	//NiColorAlpha* pColor;

	// ambient light is constant 0
	//ScaleColor(GetLightConstant(0), TheShaderManager->ShaderConst.HDR.PointLightMult);

	// pointlight registers go from 0 to 10
	//for (UInt32 i = 0; i < 12; i++) {
	//	ScaleColor(GetLightConstant(i), TheShaderManager->ShaderConst.HDR.PointLightMult);
	//}

	// emittance color is index 27
	//ScaleColor(GetLightConstant(27), TheShaderManager->ShaderConst.HDR.PointLightMult);
//}

