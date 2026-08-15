// Low health and fatigue (stamina) fullscreen shader for Oblivion/Skyrim Reloaded

string PipelinePosition = "PostTonemapping";

// TESR_LowHFData is computed each frame from the player's current health/fatigue
// percentage (see LowHFEffect::UpdateConstants()) blended with the Shaders.LowHF.Main
// settings (LumaMultiplier, BlurMultiplier, VignetteMultiplier, DarknessMultiplier,
// HealthLimit, FatigueLimit) -- it is not itself a directly user-editable setting.
// R3f-2 correction: not `packed` either -- every component multiplies a real
// setting together with the live health/fatigue percentage, so no component
// is a clean 1:1 reflection of a single SettingManager key the way `packed`
// requires. `hidden` is the honest fit; the six real settings behind this
// still render normally via their own un-annotated Shaders.LowHF.Main nodes.
float4 TESR_LowHFData
<
	string widget = "hidden";
	string name = "Low Health/Fatigue Data";
	string description = "Packed per-frame low-health/fatigue screen effect strength: x = desaturation amount, y = blur sample offset, z = vignette exponent, w = darkness multiplier. Derived from current health/fatigue and the Shaders.LowHF.Main settings; not directly user-editable.";
	float defaultValue = 0.0;
>;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

static const float3 LuminanceConv = { 0.2125f, 0.7154f, 0.0721f };

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float4 HFPass(VSOUT IN) : COLOR0
{
	float3 Color = tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb;
	
	if (TESR_LowHFData.y > 0)
	{
		Color += tex2D(TESR_RenderedBuffer, IN.UVCoord + TESR_LowHFData.y).rgb;
		Color += tex2D(TESR_RenderedBuffer, IN.UVCoord + TESR_LowHFData.y * 2).rgb;
		Color += tex2D(TESR_RenderedBuffer, IN.UVCoord - TESR_LowHFData.y).rgb;
		Color += tex2D(TESR_RenderedBuffer, IN.UVCoord - TESR_LowHFData.y * 2).rgb;
		Color = Color / 5;
	}	
	if (TESR_LowHFData.x > 0)
	{
		Color = lerp(Color, dot(Color, LuminanceConv), TESR_LowHFData.x);
	}
	if (TESR_LowHFData.z > 0)
	{
		float2 inTex = IN.UVCoord - 0.5;
		float vignette = 1 - dot(inTex, inTex);
		
		Color *= saturate(pow(abs(vignette), TESR_LowHFData.z) + TESR_LowHFData.w);
	}
	return float4(Color, 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 HFPass();
	}
}