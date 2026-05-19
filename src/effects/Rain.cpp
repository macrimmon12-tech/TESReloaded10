#include "Rain.h"

void RainEffect::UpdateConstants() {
	if (TheShaderManager->GameState.isRainy && Constants.RainAnimator.switched == false) {
		// it just started raining
		Constants.RainAnimator.switched = true;
		Constants.RainAnimator.Start(0.05f, 1);
	}
	else if (!TheShaderManager->GameState.isRainy && Constants.RainAnimator.switched) {
		// it just stopped raining
		Constants.RainAnimator.switched = false;
		Constants.RainAnimator.Start(0.07f, 0);
	}

	Constants.Data.x = Constants.RainAnimator.GetValue();

	// Derive camera velocity from position delta, smoothed to reduce jitter
	float frameTime = TheShaderManager->ShaderConst.GameTime.w;
	const D3DXVECTOR4& camPos = TheRenderManager->CameraPosition;
	if (frameTime > 0.001f) {
		D3DXVECTOR4 rawVel;
		rawVel.x = (camPos.x - Constants.PrevCamPos.x) / frameTime;
		rawVel.y = (camPos.y - Constants.PrevCamPos.y) / frameTime;
		rawVel.z = (camPos.z - Constants.PrevCamPos.z) / frameTime;
		rawVel.w = 0.0f;
		float smooth = min(1.0f, frameTime * 6.0f); // converges in ~0.17s
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

void RainEffect::UpdateSettings() {
	Constants.Data.y = TheSettingManager->GetSettingF("Shaders.Precipitations.Main", "VerticalScale");
	Constants.Data.z = TheSettingManager->GetSettingF("Shaders.Precipitations.Main", "Speed");
	Constants.Data.w = TheSettingManager->GetSettingF("Shaders.Precipitations.Main", "Opacity");

	Constants.Aspect.x = TheSettingManager->GetSettingF("Shaders.Precipitations.Main", "Refraction");
	Constants.Aspect.y = TheSettingManager->GetSettingF("Shaders.Precipitations.Main", "Coloring");
	Constants.Aspect.z = TheSettingManager->GetSettingF("Shaders.Precipitations.Main", "Bloom");
}

void RainEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_RainData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_RainAspect", &Constants.Aspect);
	TheShaderManager->RegisterConstant("TESR_RainVelocity", &Constants.Velocity);
}

bool RainEffect::ShouldRender() {
	return Constants.Data.x > 0.0f &&
		TheShaderManager->GameState.isExterior &&
		!TheShaderManager->GameState.isUnderwater;
};