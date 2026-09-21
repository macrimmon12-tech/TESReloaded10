#pragma once

class GodRaysEffect : public EffectRecord
{
public:
	GodRaysEffect() : EffectRecord("GodRays") {
		selectedTechnique = 0; // Classic, until the first settings pass picks a real value
	};

	struct GodRaysStruct {
		D3DXVECTOR4		Ray;
		D3DXVECTOR4		RayColor;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Enhanced; // x: RayDecay, y: RayStepScale, z: BlurStrength, w: GlareStrength
	};
	GodRaysStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();

	float dayMult;
	float nightMult;
	bool sunGlareEnabled;
	float rayVisibility;

	// which of GodRays.fx.hlsl's named techniques to render (0: Classic, 1: Enhanced,
	// 2: GlareOnly -- reserved for when the fog-integrated Volumetric light-shafts tier is active
	// elsewhere and streak duty moves there), mirroring FlashlightEffect::selectedPass
	int selectedTechnique;
};