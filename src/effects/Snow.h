#pragma once

class SnowEffect : public EffectRecord
{
public:
	SnowEffect() : EffectRecord("Snow") {
		Constants.SnowAnimator.Initialize(0);
	};

	struct SnowStruct {
		Animator		SnowAnimator;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Velocity;    // smoothed world-space camera velocity (xyz), speed magnitude (w)
		D3DXVECTOR4		PrevCamPos;  // previous frame camera position for velocity derivation
	};
	SnowStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();

};