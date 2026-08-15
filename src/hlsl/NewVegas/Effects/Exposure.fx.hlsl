// Exposure shader for Oblivion/Skyrim Reloaded
string PipelinePosition = "PreTonemapping";

#define debug 0

float4 TESR_ExposureData
<
	string name = "Exposure Data";
	string description = "Packed exposure parameters (Shaders.Exposure.Main/Night/Interiors, day/night/interior blended): x = MinBrightness, y = MaxBrightness, z = DarkAdaptSpeed, w = LightAdaptSpeed.";
	float defaultValue = 0.2;
>;
float4 TESR_DebugVar
<
	string widget = "hidden";
	string name = "Debug Variable";
	string description = "Developer scratch variable (Main.Develop.Main.DebugVar1-4). Not intended for normal use.";
	float defaultValue = 0.0;
>;
//float4 TESR_FrameTime; // x:min brightness, y;max brightness, z:dark adapt speed, w: light adapt speed

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_AvgLumaBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"

static const float minBrightness = TESR_ExposureData.x;
static const float maxBrightness = TESR_ExposureData.y;

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


float4 Exposure(VSOUT IN) : COLOR0
{
	float4 color = linearize(tex2D(TESR_RenderedBuffer, IN.UVCoord));
	float averageLuma = tex2D(TESR_AvgLumaBuffer, float2(0.5, 0.5)).g;

	float negativeLumaDiff = max(log(minBrightness / averageLuma), 0) + 1;
	float additiveLumaDiff = max(log(averageLuma / minBrightness), 0) + 1;

	//float additiveLumaDiff = invlerps(maxBrightness, 1, averageLuma) * (1 - maxBrightness) * 2;
	float lumaDiff = additiveLumaDiff / negativeLumaDiff;
	// float lumaDiff = invlerps(minBrightness, maxBrightness, averageLuma);


#if debug
	if (IN.UVCoord.x > 0.7 && IN.UVCoord.x < 0.8 && IN.UVCoord.y>0.7 && IN.UVCoord.y<0.8) return averageLuma;
	if (IN.UVCoord.x > 0.7 && IN.UVCoord.x < 0.8 && IN.UVCoord.y>0.8 && IN.UVCoord.y<0.9) return float4(additiveLumaDiff, negativeLumaDiff, 0, 1);
#endif

	// color = pows(color, (1 + lumaDiff));
	return delinearize(float4(lerp(color.rgb, color.rgb / lumaDiff, maxBrightness), 1));
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 Exposure();
	}
}