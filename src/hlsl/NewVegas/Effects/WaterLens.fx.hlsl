// WaterLens fullscreen shader for Oblivion Reloaded

string PipelinePosition = "PostTonemapping";

float4 TESR_ReciprocalResolution
<
	string widget = "hidden";
	string name = "Reciprocal Resolution";
	string description = "Per-frame render target metrics supplied by the engine: x = 1/width, y = 1/height, z = aspect ratio (width/height), w = reserved for FoV. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_GameTime
<
	string widget = "hidden";
	string name = "Game Time";
	string description = "Per-frame game clock supplied by the engine: x = time in milliseconds, y = time in hours (0-24), z = frame time counter used to animate the lens ripple, w = elapsed time in seconds since last frame. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_WaterLensData
<
	string widget = "packed";
	string name = "Water Lens Data";
	string description = "Packed water-lens ripple parameters: x = TimeMultA (Shaders.WaterLens.Main.TimeMultA, first noise animation speed), y = TimeMultB (TimeMultB, second noise animation speed), z = Viscosity (refraction distortion amount), w = current effect amount (Amount setting scaled by the underwater fade-out animator).";
	string componentKeys     = "TimeMultA,TimeMultB,Viscosity,Amount";
	string componentDefaults = "0.1,0.2,0.05,0.1";
	string componentMins     = "0,0,0,0";
	string componentMaxs     = "1,1,1,1";
	string componentSteps    = "0.01,0.01,0.01,0.01";
	float defaultValue = 0.1;
>;

struct VS_OUTPUT
{
	float4 pos : POSITION0;
	float2 texCoord  : TEXCOORD0;
	float4 noiseCoord : TEXCOORD1;
};

struct PS_INPUT
{
	float2 texCoord : TEXCOORD0;
	float4 noiseCoord : TEXCOORD1;
};

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_waterlensSampler : register(s1) < string ResourceName = "Effects\water_NRM_lens.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

static const float timea = TESR_GameTime.z * TESR_WaterLensData.x;
static const float timeb = TESR_GameTime.z * TESR_WaterLensData.y;

void convToImageSpace (float4 position, out float4 oPosition, out float2 oTexCoord)
{
	position.xy = sign(position.xy);
	oPosition = float4(position.xy, 0, 1);
	oTexCoord.x = 0.5 * (1 + position.x) + 0.5 / (1.0 / TESR_ReciprocalResolution.x);
	oTexCoord.y = 0.5 * (1 - position.y) + 0.5 / (1.0 / TESR_ReciprocalResolution.y);
}

VS_OUTPUT WaterLensVS (float4 position : POSITION)
{
	VS_OUTPUT Output = (VS_OUTPUT)0;
	
	convToImageSpace(position, Output.pos, Output.texCoord);
	Output.noiseCoord.xy = (Output.texCoord * 0.5f) + float2(sin(0.25f * timea), -0.25f * timea);
	Output.noiseCoord.zw = Output.texCoord - float2(0.0f, timeb);

	return Output;
}

float4 WaterLensPS (PS_INPUT Input) : COLOR
{
	float4 normalColor = (tex2D(TESR_waterlensSampler, Input.noiseCoord.xy) * 2.0f) - 1.0f;
	float4 animColor = (tex2D(TESR_waterlensSampler, Input.noiseCoord.zw) * 2.0f) - 1.0f;

	normalColor.z += animColor.w;
	float3 normal = normalize(normalColor.xyz);
	float3 refractionVec = normalize(normal * float3(TESR_WaterLensData.z, TESR_WaterLensData.z, 1.0f));
	float3 screenColor = tex2D(TESR_RenderedBuffer, Input.texCoord - (refractionVec.xy * 0.2f * TESR_WaterLensData.w)).xyz;
	float2 refractHighlight = refractionVec.xy * TESR_WaterLensData.w;
	float3 refColor = (saturate(pow(refractHighlight.x, 6.0f)) + saturate(pow(refractHighlight.y, 6.0f))) * float3(0.85f, 0.85f, 1.0f);

	return float4(screenColor + refColor, 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 WaterLensVS();
		PixelShader = compile ps_3_0 WaterLensPS();
	}
}
