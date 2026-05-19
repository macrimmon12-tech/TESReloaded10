#pragma once

class RainEffect : public EffectRecord
{
public:
	RainEffect() : EffectRecord("Precipitations") {
		Constants.Data = D3DXVECTOR4(0, 0, 0, 0);
		Constants.RainAnimator.Initialize(0);
	};

	struct RainStruct {
		Animator		RainAnimator;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Aspect;
		D3DXVECTOR4		Velocity;    // smoothed world-space camera velocity (xyz), speed magnitude (w)
		D3DXVECTOR4		PrevCamPos;  // previous frame camera position for velocity derivation
	};
	RainStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();
};