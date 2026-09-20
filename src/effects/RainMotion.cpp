#include "RainMotion.h"

RainMotionEffect::~RainMotionEffect() {
	ReleaseGeometry();
}

/*
* Releases the streak vertex/index buffers, if any were built.
*/
void RainMotionEffect::ReleaseGeometry() {
	if (StreakVertexBuffer) { StreakVertexBuffer->Release(); StreakVertexBuffer = nullptr; }
	if (StreakIndexBuffer) { StreakIndexBuffer->Release(); StreakIndexBuffer = nullptr; }
	BuiltStreakCount = 0;
}

/*
* Builds a static instance buffer: 4 vertices / 6 indices per streak, carrying only a local
* quad corner (x = -1/1 width side, y = 0/1 leading/trailing edge) and the streak's own instance
* index. Nothing else about a streak's placement is stored -- the vertex shader derives a
* deterministic pseudo-random local offset purely by hashing that index, so there is no per-
* instance CPU data to keep in sync and no ceiling imposed by shader-constant register space
* (unlike the engine's own grass/SpeedTree "poor-man's instancing", which needs a real per-
* instance constant array because blade positions are authored, not procedural).
*/
void RainMotionEffect::BuildGeometry(int streakCount) {
	ReleaseGeometry();
	if (streakCount <= 0) return;

	struct StreakVertex { float cornerX, cornerY, instanceIndex; };

	std::vector<StreakVertex> vertices(streakCount * 4);
	std::vector<UInt16> indices(streakCount * 6);

	for (int i = 0; i < streakCount; i++) {
		float idx = (float)i;
		StreakVertex* v = &vertices[i * 4];
		v[0] = { -1.0f, 0.0f, idx }; // leading edge, left
		v[1] = { 1.0f, 0.0f, idx };  // leading edge, right
		v[2] = { -1.0f, 1.0f, idx }; // trailing edge, left
		v[3] = { 1.0f, 1.0f, idx };  // trailing edge, right

		UInt16 base = (UInt16)(i * 4);
		UInt16* ix = &indices[i * 6];
		ix[0] = base + 0; ix[1] = base + 1; ix[2] = base + 2;
		ix[3] = base + 2; ix[4] = base + 1; ix[5] = base + 3;
	}

	IDirect3DDevice9* Device = TheRenderManager->device;

	Device->CreateVertexBuffer((UINT)(vertices.size() * sizeof(StreakVertex)), D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &StreakVertexBuffer, NULL);
	void* VertexData = NULL;
	StreakVertexBuffer->Lock(0, 0, &VertexData, NULL);
	memcpy(VertexData, vertices.data(), vertices.size() * sizeof(StreakVertex));
	StreakVertexBuffer->Unlock();

	Device->CreateIndexBuffer((UINT)(indices.size() * sizeof(UInt16)), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &StreakIndexBuffer, NULL);
	void* IndexData = NULL;
	StreakIndexBuffer->Lock(0, 0, &IndexData, NULL);
	memcpy(IndexData, indices.data(), indices.size() * sizeof(UInt16));
	StreakIndexBuffer->Unlock();

	BuiltStreakCount = streakCount;
}

void RainMotionEffect::UpdateSettings() {
	int streakCount = TheSettingManager->GetSettingI("Shaders.RainMotion.Main", "StreakDensity");
	streakCount = std::clamp(streakCount, 0, 8000); // 4 verts/streak, stay well under the 65535 cap of a 16-bit index buffer

	Settings.StreakCount = streakCount;
	Settings.FallSpeed = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "FallSpeed");
	Settings.StreakLength = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "StreakLength");
	Settings.StreakWidth = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "StreakWidth");
	Settings.VolumeSizeXY = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "VolumeSizeXY");
	Settings.VolumeSizeZ = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "VolumeSizeZ");
	Settings.WindDriftSpeed = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "WindDriftSpeed");
	Settings.WindGustStrength = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "WindGustStrength");
	Settings.PlayerWindInfluence = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "PlayerWindInfluence");
	Settings.CameraWhipShear = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "CameraWhipShear");
	Settings.RefractionStrength = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "RefractionStrength");
	Settings.FadeStart = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "FadeStart");
	Settings.FadeRange = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "FadeRange");
	Settings.Opacity = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "Opacity");
	Settings.TeleportSpeedThreshold = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "TeleportSpeedThreshold");
	Settings.IntensityIncreaseRate = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "IntensityIncreaseRate");
	Settings.IntensityDecreaseRate = TheSettingManager->GetSettingF("Shaders.RainMotion.Main", "IntensityDecreaseRate");

	if (streakCount != BuiltStreakCount) BuildGeometry(streakCount);
}

void RainMotionEffect::UpdateConstants() {
	// Intensity fade: same isRainy edge-trigger + Animator idiom WetWorld/WaterLens/Rain each use
	// independently -- a generic, reusable pattern, not shared state with any of them.
	bool isRainy = TheShaderManager->GameState.isRainy;
	if (isRainy && !Constants.IntensityAnimator.switched) {
		Constants.IntensityAnimator.switched = true;
		Constants.IntensityAnimator.Start(Settings.IntensityIncreaseRate, 1.0f);
	}
	else if (!isRainy && Constants.IntensityAnimator.switched) {
		Constants.IntensityAnimator.switched = false;
		Constants.IntensityAnimator.Start(Settings.IntensityDecreaseRate, 0.0f);
	}
	float intensity = Constants.IntensityAnimator.GetValue();

	// Player velocity: PlayerCharacter exposes no velocity field, so derive it from a raw
	// position delta, smoothed over 3 frames the same way MotionBlurEffect smooths its own
	// rotation delta. isCellChanged guards the one-frame spike a teleport/fast-travel/door
	// load would otherwise inject; TeleportSpeedThreshold catches anything else that slips
	// through (e.g. scripted moves that don't flip isCellChanged).
	NiPoint3 pos = Player->pos;
	float dt = (float)TheFrameRateManager->ElapsedTime;
	D3DXVECTOR2 instVelocity(0.0f, 0.0f);
	if (Constants.havePrevPos && !TheShaderManager->GameState.isCellChanged && dt > 0.0001f) {
		NiPoint3 delta = pos - Constants.prevPlayerPos;
		instVelocity.x = delta.x / dt;
		instVelocity.y = delta.y / dt;
		float instSpeed = sqrtf(instVelocity.x * instVelocity.x + instVelocity.y * instVelocity.y);
		if (instSpeed > Settings.TeleportSpeedThreshold) instVelocity = D3DXVECTOR2(0.0f, 0.0f);
	}
	else {
		Constants.oldVelocity = D3DXVECTOR2(0.0f, 0.0f);
		Constants.oldoldVelocity = D3DXVECTOR2(0.0f, 0.0f);
	}
	Constants.velocity.x = (Constants.oldoldVelocity.x + Constants.oldVelocity.x + instVelocity.x) / 3.0f;
	Constants.velocity.y = (Constants.oldoldVelocity.y + Constants.oldVelocity.y + instVelocity.y) / 3.0f;
	Constants.oldoldVelocity = Constants.oldVelocity;
	Constants.oldVelocity = instVelocity;
	Constants.prevPlayerPos = pos;
	Constants.havePrevPos = true;

	// Camera yaw delta: identical wrap-safe diff MotionBlurEffect uses to handle the 0/360
	// degree seam, kept as this effect's own independent state.
	float AngleZ = D3DXToDegree(Player->rot.z);
	float deltaZ = Constants.oldAngleZ - AngleZ;
	float wrapA = deltaZ + 360.0f;
	float wrapB = (AngleZ - Constants.oldAngleZ + 360.0f) * -1.0f;
	if (fabsf(deltaZ) > fabsf(wrapA)) deltaZ = wrapA;
	else if (fabsf(deltaZ) > fabsf(wrapB)) deltaZ = wrapB;
	float smoothedAngular = (Constants.oldoldAngularDelta + Constants.oldAngularDelta + deltaZ) / 3.0f;
	Constants.oldoldAngularDelta = Constants.oldAngularDelta;
	Constants.oldAngularDelta = deltaZ;
	Constants.oldAngleZ = AngleZ;

	// Procedural wind: a self-contained, slowly drifting heading + gust envelope built purely
	// from elapsed time (mismatched sine frequencies so it never visibly loops). No game wind
	// data is used -- FNV's Sky::windSpeed/windDirection fields aren't meaningfully populated.
	float t = TheShaderManager->ShaderConst.GameTime.z;
	float windDir = 0.35f * sinf(0.021f * t + 1.7f) + 0.5f * sinf(0.0073f * t + 4.1f);
	float gust = 0.5f + 0.5f * sinf(0.037f * t + 0.6f) * sinf(0.011f * t + 2.3f);
	float windMag = Settings.WindDriftSpeed + Settings.WindGustStrength * gust;
	D3DXVECTOR2 proceduralWind(cosf(windDir) * windMag, sinf(windDir) * windMag);

	// Relative wind = procedural wind opposed by player motion -- real world-space vectors,
	// stable at any camera heading (unlike an angle derived from the camera's own facing).
	D3DXVECTOR2 relativeWind;
	relativeWind.x = proceduralWind.x - Constants.velocity.x * Settings.PlayerWindInfluence;
	relativeWind.y = proceduralWind.y - Constants.velocity.y * Settings.PlayerWindInfluence;
	float relativeWindMag = sqrtf(relativeWind.x * relativeWind.x + relativeWind.y * relativeWind.y);

	float fallSpeedEffective = Settings.FallSpeed * (1.0f + 0.001f * relativeWindMag);
	float fx = relativeWind.x, fy = relativeWind.y, fz = -fallSpeedEffective;
	float flen = sqrtf(fx * fx + fy * fy + fz * fz);
	if (flen < 0.0001f) flen = 1.0f;

	Constants.Fall.x = fx / flen;
	Constants.Fall.y = fy / flen;
	Constants.Fall.z = fz / flen;
	Constants.Fall.w = smoothedAngular * Settings.CameraWhipShear; // signed camera-whip shear

	Constants.Data.x = intensity;
	Constants.Data.y = fallSpeedEffective;
	Constants.Data.z = Settings.StreakLength;
	Constants.Data.w = Settings.StreakWidth;

	Constants.Volume.x = Settings.VolumeSizeXY;
	Constants.Volume.y = Settings.VolumeSizeXY;
	Constants.Volume.z = Settings.VolumeSizeZ;
	Constants.Volume.w = (float)Settings.StreakCount;

	Constants.Fade.x = Settings.FadeStart;
	Constants.Fade.y = Settings.FadeRange;
	Constants.Fade.z = Settings.RefractionStrength;
	Constants.Fade.w = Settings.Opacity;
}

void RainMotionEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_RainMotionData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_RainMotionFall", &Constants.Fall);
	TheShaderManager->RegisterConstant("TESR_RainMotionVolume", &Constants.Volume);
	TheShaderManager->RegisterConstant("TESR_RainMotionFade", &Constants.Fade);
}

bool RainMotionEffect::ShouldRender() {
	return Constants.Data.x > 0.0f &&
		TheShaderManager->GameState.isExterior &&
		!TheShaderManager->GameState.isUnderwater;
}

/*
* Overrides the base fullscreen-quad Render(): this effect draws real instanced-by-hash streak
* geometry instead, so it needs its own vertex/index stream rather than the shared FrameVertex
* quad every other post effect uses. The stream/FVF is restored before returning so every effect
* rendered after this one in the pipeline still finds the fullscreen quad bound, the same way
* BloomEffect::RenderBloomBuffer restores it after using its own per-mip vertex buffers.
*/
void RainMotionEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || Effect == nullptr || !ShouldRender() || StreakVertexBuffer == nullptr || StreakIndexBuffer == nullptr || BuiltStreakCount == 0) {
		renderTime = 0.0f;
		return;
	}

	auto timer = TimeLogger();
	if (SourceBuffer) Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_LINEAR);

	Device->SetStreamSource(0, StreakVertexBuffer, 0, sizeof(float) * 3);
	Device->SetFVF(D3DFVF_XYZ);
	Device->SetIndices(StreakIndexBuffer);

	try {
		D3DXHANDLE technique = Effect->GetTechnique(techniqueIndex);
		Effect->SetTechnique(technique);
		SetCT();
		UINT Passes;
		Effect->Begin(&Passes, NULL);
		for (UINT p = 0; p < Passes; p++) {
			if (ClearRenderTarget) Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0L);
			Effect->BeginPass(p);
			Device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, BuiltStreakCount * 4, 0, BuiltStreakCount * 2);
			Effect->EndPass();
			if (RenderedSurface) Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_LINEAR);
		}
		Effect->End();
	}
	catch (const std::exception& e) {
		Logger::Log("Error during rendering of effect %s: %s", Name, e.what());
	}

	Device->SetStreamSource(0, TheShaderManager->FrameVertex, 0, sizeof(FrameVS));
	Device->SetFVF(FrameFVF);

	std::string name = "EffectRecord::Render " + std::string(Name);
	renderTime = timer.LogTime(name.c_str());
}
