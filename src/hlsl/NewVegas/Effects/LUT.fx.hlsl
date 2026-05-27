// LUT color grading effect for New Vegas Reloaded
// Supports 256x16 (N=16), 1024x32 (N=32), and 4096x64 (N=64) strip LUT textures.
// N is the cell size/count and is passed via TESR_LUTData.x at runtime.

float4 TESR_LUTData;   // x=N (cell size = cell count), y=strength
float4 TESR_LUTBlend;  // x=dayNightLerp (0=night, 1=day), y=isInterior (0 or 1)

sampler2D TESR_RenderedBuffer    : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LUTDayBuffer      : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LUTNightBuffer    : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LUTInteriorBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

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

// Sample a horizontal-strip LUT texture.
// N = cell count = cell size (e.g. 16 for 256x16, 32 for 1024x32, 64 for 4096x64).
// Texture layout: N cells wide, each cell N pixels square, blue axis left-to-right.
float3 SampleLUT(sampler2D lut, float3 color, float N)
{
	float b      = color.b * (N - 1.0);
	float bCell  = floor(b);
	float bFrac  = frac(b);

	float invW = 1.0 / (N * N); // 1 / textureWidth
	float invH = 1.0 / N;       // 1 / textureHeight

	float2 uv1, uv2;
	uv1.x = (bCell       * N + color.r * (N - 1.0) + 0.5) * invW;
	uv1.y = (                  color.g * (N - 1.0) + 0.5) * invH;
	uv2.x = ((bCell + 1.0) * N + color.r * (N - 1.0) + 0.5) * invW;
	uv2.y = uv1.y;

	return lerp(tex2D(lut, uv1).rgb, tex2D(lut, uv2).rgb, bFrac);
}

float4 LUTPass(VSOUT IN) : COLOR0
{
	float3 color     = tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb;
	float  N         = TESR_LUTData.x;
	float  strength  = TESR_LUTData.y;
	float  hdrCompat = TESR_LUTBlend.z; // 0=off, 1=max-channel normalization for SDR LUTs on HDR input

	// HDR compat: normalize so the brightest channel = 1.0 before sampling the LUT,
	// then restore the scale afterwards. For SDR pixels (all channels <= 1) scale=1, no change.
	float  scale    = max(max(color.r, color.g), max(color.b, 1.0));
	float3 lutInput = color / lerp(1.0, scale, hdrCompat);

	float3 dayColor      = SampleLUT(TESR_LUTDayBuffer,      lutInput, N);
	float3 nightColor    = SampleLUT(TESR_LUTNightBuffer,    lutInput, N);
	float3 interiorColor = SampleLUT(TESR_LUTInteriorBuffer, lutInput, N);

	float3 exteriorColor = lerp(nightColor, dayColor, TESR_LUTBlend.x);
	float3 graded        = lerp(exteriorColor, interiorColor, TESR_LUTBlend.y);

	graded *= lerp(1.0, scale, hdrCompat); // restore HDR luminance scale if hdrCompat active

	return float4(lerp(color, graded, strength), 1.0f);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 LUTPass();
	}
}
