// Lens Shader For TESReloaded
//--------------------------------------------------

string PipelinePosition = "PreTonemapping";

float4 TESR_ReciprocalResolution
<
	string name = "Reciprocal Resolution";
	string description = "Per-frame render target metrics supplied by the engine: x = 1/width, y = 1/height, z = aspect ratio (width/height), w = reserved for FoV. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_CinemaSettings
<
	string name = "Cinema Settings";
	string description = "CinemaEffect's own registered constant (see Cinema.fx.hlsl): x = dirt lens opacity, y = grain amount, z = chromatic aberration strength.";
	float defaultValue = 1.0;
>;
float4 TESR_SunColor
<
	string name = "Sun Color";
	string description = "Current directional sunlight color (RGB), supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunAmbient
<
	string name = "Sun Ambient";
	string description = "Current ambient sky light color (RGB), supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunAmount
<
	string name = "Sun Amount";
	string description = "Day/night blend amount supplied by the engine, used to fade effects across sunrise/sunset. Not user-configurable.";
	float defaultValue = 0.0;
>;
// Comment above previously read "x: lens strength, y: luma threshold" -- corrected
// against LensEffect::UpdateConstants(), which packs three settings, not two.
float4 TESR_LensData
<
	string name = "Lens Data";
	string description = "Packed dirt-lens parameters (Shaders.Lens.Main/Night/Interiors, day/night/interior blended): x = strength (overall effect strength), y = bloomExponent (how far dirt particles are lit from the light source), z = smudginess (texture scale).";
	float defaultValue = 0.4;
>;
float4 TESR_DebugVar
<
	string name = "Debug Variable";
	string description = "Developer scratch variable (Main.Develop.Main.DebugVar1-4), used here for the luma threshold feeding the bloom mask. Not intended for normal use.";
	float defaultValue = 0.0;
>;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_LensSampler : register(s3) < string ResourceName = "Effects\dirtlens.png"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

//static const float scale = 0.5;

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

#include "Includes/Helpers.hlsl"

float4 Lens(VSOUT IN) : COLOR0 
{
	float2 uv = IN.UVCoord;
    float4 color = linearize(tex2D(TESR_SourceBuffer, uv));
    float4 dirtColor = pows(tex2D(TESR_LensSampler, uv), max(0, 3 - TESR_LensData.z));

    // Get the bloom mask to calculate areas where dirt lens will appear
	float4 bloom = tex2D(TESR_BloomBuffer, IN.UVCoord);
    float bloomLuma = luma(bloom);
    bloom = pows(bloomLuma, TESR_LensData.y) * (bloom / bloomLuma);
    color += dirtColor.r * bloom * TESR_LensData.x;

    return delinearize(float4(color.rgb, 1));
}

technique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Lens();
    }
}