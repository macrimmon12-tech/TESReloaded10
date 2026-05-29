#pragma once

class DepthOfFieldEffect : public EffectRecord
{
public:
	DepthOfFieldEffect() : EffectRecord("DepthOfField") {};

	struct ValuesStruct {
		float DistantBlur;
		float DistantBlurStartRange;
		float DistantBlurEndRange;
		float BlurRadius;
		float FocusRange;
		float NearBlurCutOff;
		float BokehCurve;
		int   SampleCount;
		float ChromaticAberration;
		float AnamorphicRatio;
		int   Mode;
		bool  Enabled;
	};

	struct DepthOfFieldSettingsStruct {
		ValuesStruct	FirstPerson;
		ValuesStruct	ThirdPerson;
		ValuesStruct	VanityView;
	};
	DepthOfFieldSettingsStruct Settings;

	struct DepthOfFieldStruct {
		bool			Enabled;
		D3DXVECTOR4		Blur;       // x: distant blur, y: distant start, z: distant end, w: blur radius
		D3DXVECTOR4		Data;       // x: focus range, y: (unused), z: (unused), w: near blur cutoff
		D3DXVECTOR4		MatsoData;  // x: BokehCurve, y: SampleCount, z: ChromaticAberration, w: AnamorphicRatio
	};
	DepthOfFieldStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();
};