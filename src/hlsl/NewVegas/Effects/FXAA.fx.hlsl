// Lean FXAA for NVR ([Main.Main.ReducedQuality] FXAA).
// Console-style FXAA, after FXAA 3.11 by Timothy Lottes: four half-texel diagonal taps (each a
// bilinear average of a 2x2 block) measure local contrast and the edge direction. Pixels below
// the contrast threshold are returned unchanged after five fetches; edge pixels average two or
// four taps along the edge (nine fetches). Runs on the tonemapped, gamma-space image.

float4 TESR_ReciprocalResolution;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

static const float EdgeThreshold = 0.125;   // minimum contrast, relative to the local maximum luma
static const float EdgeThresholdMin = 0.05; // absolute minimum contrast (leaves dark areas alone)
static const float EdgeSharpness = 8.0;     // shortens the wide taps on edges near an axis

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

float Luma(float3 color)
{
	return dot(color, float3(0.299, 0.587, 0.114));
}

// Explicit LOD: most fetches sit inside the dynamic branch below.
float3 Fetch(float2 uv)
{
	return tex2Dlod(TESR_RenderedBuffer, float4(uv, 0, 0)).rgb;
}

float4 FXAA(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float2 texel = TESR_ReciprocalResolution.xy;

	float3 center = Fetch(uv);
	float lumaM = Luma(center);
	float lumaNW = Luma(Fetch(uv + float2(-0.5, -0.5) * texel));
	float lumaNE = Luma(Fetch(uv + float2( 0.5, -0.5) * texel)) + 1.0 / 384.0; // breaks exact ties
	float lumaSW = Luma(Fetch(uv + float2(-0.5,  0.5) * texel));
	float lumaSE = Luma(Fetch(uv + float2( 0.5,  0.5) * texel));

	float lumaMin = min(min(lumaNW, lumaNE), min(lumaSW, lumaSE));
	float lumaMax = max(max(lumaNW, lumaNE), max(lumaSW, lumaSE));
	float range = max(lumaMax, lumaM) - min(lumaMin, lumaM);
	[branch] if (range < max(EdgeThresholdMin, lumaMax * EdgeThreshold)) return float4(center, 1.0);

	// The luma gradient points across the edge; blend perpendicular to it, along the edge.
	float swMinusNE = lumaSW - lumaNE;
	float seMinusNW = lumaSE - lumaNW;
	float2 dir = float2(swMinusNE + seMinusNW, swMinusNE - seMinusNW);
	float2 dir1 = dir * rsqrt(max(dot(dir, dir), 1.0e-8));
	float3 rgbA = Fetch(uv - dir1 * 0.5 * texel) + Fetch(uv + dir1 * 0.5 * texel);

	float2 dir2 = clamp(dir1 / max(min(abs(dir1.x), abs(dir1.y)) * EdgeSharpness, 1.0e-4), -2.0, 2.0);
	float3 rgbB = (Fetch(uv - dir2 * 2.0 * texel) + Fetch(uv + dir2 * 2.0 * texel)) * 0.25 + rgbA * 0.25;

	// If the wide taps reached past the local luma range they crossed another edge: use the near pair.
	float lumaB = Luma(rgbB);
	float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA * 0.5 : rgbB;
	return float4(result, 1.0);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 FXAA();
	}
}
