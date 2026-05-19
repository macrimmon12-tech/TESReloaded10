#include "Snow.h"

void SnowEffect::UpdateConstants() {
	// Snow fall
	if (TheShaderManager->GameState.isSnow && Constants.SnowAnimator.switched == false) {
		// it just started snowing
		TheShaderManager->Effects.WetWorld->Constants.PuddlesAnimator.Start(0.3f, 0.f); // fade out any puddles if they exist
		Constants.SnowAnimator.switched = true;
		Constants.SnowAnimator.Initialize(0.f);
		Constants.SnowAnimator.Start(0.5f, 1.f);
	}
	else if (!TheShaderManager->GameState.isSnow && Constants.SnowAnimator.switched) {
		// it just stopped snowing
		Constants.SnowAnimator.switched = false;
		Constants.SnowAnimator.Start(0.2f, 0.f);
	}
	Constants.Data.x = Constants.SnowAnimator.GetValue();

	if (Constants.Data.x) TheShaderManager->orthoRequired = true; // mark ortho map calculation as necessary

	// Derive camera velocity from position delta, smoothed to reduce jitter
	float frameTime = TheShaderManager->ShaderConst.GameTime.w;
	const D3DXVECTOR4& camPos = TheRenderManager->CameraPosition;
	if (frameTime > 0.001f) {
		D3DXVECTOR4 rawVel;
		rawVel.x = (camPos.x - Constants.PrevCamPos.x) / frameTime;
		rawVel.y = (camPos.y - Constants.PrevCamPos.y) / frameTime;
		rawVel.z = (camPos.z - Constants.PrevCamPos.z) / frameTime;
		rawVel.w = 0.0f;
		float smooth = min(1.0f, frameTime * 6.0f);
		Constants.Velocity.x += (rawVel.x - Constants.Velocity.x) * smooth;
		Constants.Velocity.y += (rawVel.y - Constants.Velocity.y) * smooth;
		Constants.Velocity.z += (rawVel.z - Constants.Velocity.z) * smooth;
		Constants.Velocity.w = sqrtf(
			Constants.Velocity.x * Constants.Velocity.x +
			Constants.Velocity.y * Constants.Velocity.y +
			Constants.Velocity.z * Constants.Velocity.z
		);
	}
	Constants.PrevCamPos = camPos;
}

void SnowEffect::UpdateSettings() {
	Constants.Data.z = TheSettingManager->GetSettingF("Shaders.Snow.Main", "Speed");
}

void SnowEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_SnowData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_SnowVelocity", &Constants.Velocity);
}

bool SnowEffect::ShouldRender() {
	return Constants.Data.x > 0.0f &&
		TheShaderManager->GameState.isExterior &&
		!TheShaderManager->GameState.isUnderwater;
};