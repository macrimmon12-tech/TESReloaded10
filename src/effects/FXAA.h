#pragma once

// Lean FXAA pass (Effects\FXAA.fx.hlsl), driven by [Main.Main.ReducedQuality] FXAA rather than a
// Shaders.FXAA section. The game's own MSAA (iMultiSample) is left to the player.
class FXAAEffect : public EffectRecord
{
public:
	FXAAEffect() : EffectRecord("FXAA") {};

	bool	ShouldRender();
};
