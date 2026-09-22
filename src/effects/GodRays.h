#pragma once

class GodRaysEffect : public EffectRecord
{
public:
	GodRaysEffect() : EffectRecord("GodRays") {
		quality = 0; // Classic, until the first settings pass picks a real value
	};

	struct GodRaysStruct {
		D3DXVECTOR4		Ray;
		D3DXVECTOR4		RayColor;
		D3DXVECTOR4		Data;
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

	// 0: Classic (this effect's own single technique). 1: CrepuscularRays (a separate effect --
	// see CrepuscularRaysEffect). ShaderManager reads this to decide which of the two actually
	// renders each frame; GodRays' own Render() call always uses technique index 0 now that
	// Classic is the only technique left in GodRays.fx.hlsl.
	int quality;
};
