// Motion Blur fullscreen shader for Oblivion/Skyrim Reloaded

string PipelinePosition = "PostTonemapping";

#define BlurSamples 24

float4 TESR_ReciprocalResolution
<
	string name = "Reciprocal Resolution";
	string description = "Per-frame render target metrics supplied by the engine: x = 1/width, y = 1/height, z = aspect ratio (width/height), w = reserved for FoV. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_MotionBlurParams
<
	string name = "Motion Blur Params";
	string description = "Packed blur-kernel parameters (Shaders.MotionBlur.FirstPersonView/ThirdPersonView): x = GaussianWeight, y = BlurScale, z = BlurOffsetMax.";
	float defaultValue = 6.0;
>;
float4 TESR_MotionBlurData
<
	string name = "Motion Blur Data";
	string description = "Packed per-frame camera-turn blur amount: x = smoothed horizontal (yaw) blur amount, y = smoothed vertical (pitch) blur amount. Derived each frame from player rotation delta; not directly user-editable.";
	float defaultValue = 0.0;
>;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

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

float4 MotionBlurPS(VSOUT IN) : COLOR0
{
    float2 offsetBlurDirection;
    float3 blurredColor = 0;
	float gaussianSum = 0;
	
    offsetBlurDirection.x = clamp(TESR_MotionBlurData.x, -TESR_MotionBlurParams.z, TESR_MotionBlurParams.z);
    offsetBlurDirection.y = clamp(TESR_MotionBlurData.y, -TESR_MotionBlurParams.z, TESR_MotionBlurParams.z);

    for(int i = 0; i < BlurSamples / 2; i++)
    {
        float2 lookup;
        float4 offsetColor;
        float weight = exp((i*i) / (5.0 * TESR_MotionBlurParams.x * TESR_MotionBlurParams.x)) / TESR_MotionBlurParams.x;
 
        lookup = offsetBlurDirection * TESR_ReciprocalResolution.xy * TESR_MotionBlurParams.y;
        lookup = lookup * i / BlurSamples + IN.UVCoord; 
        offsetColor = tex2D(TESR_RenderedBuffer, lookup);
        gaussianSum += weight;
        blurredColor += (offsetColor.rgb * weight);
 
        lookup = -offsetBlurDirection * TESR_ReciprocalResolution.xy * TESR_MotionBlurParams.y;
        lookup = lookup * i / BlurSamples + IN.UVCoord;
        offsetColor = tex2D(TESR_RenderedBuffer, lookup);
        gaussianSum += weight;
        blurredColor += (offsetColor.rgb * weight);
    }
 
	blurredColor = blurredColor / gaussianSum;
	return float4(blurredColor, 1);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MotionBlurPS();
	}
 
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MotionBlurPS();
	}
}
