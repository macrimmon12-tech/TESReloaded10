// Image space shadows shader for Oblivion Reloaded

float4x4 TESR_WorldTransform;
float4 TESR_ShadowData;
float4 TESR_ShadowFade;
float4 TESR_ReciprocalResolution;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_PointShadowBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
static const float MAXDISTANCE = TESR_ShadowFade.w;

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

#include "Includes/BlurDepth.hlsl"


// Applies the blurred shadow term to the scene colour. Shared by both techniques below.
float4 ApplyInteriorShadow(float2 uv, float4 color, float shadowSample) {
    color.rgb = pows(color.rgb, 2.2); // linearise
	float depth = readDepth(uv);
	float3 eyeDir = toWorld(uv);
	float uniformDepth = length(depth * eyeDir);

	float Shadow = saturate(shadowSample);
	Shadow = lerp(Shadow, 1.0, invlerps(300, MAXDISTANCE, uniformDepth)); // fade shadows with distance
	float darkness = scaledReinhard(TESR_ShadowData.y, 5.0);
    darkness = saturate(1 - darkness * TESR_ShadowFade.y); // shadowFade.y is set to 0 when shadows are disabled

	Shadow = saturate(lerp(darkness, 1, Shadow)); // shadow darkness to apply in the scene - 0 is full shadow 1 is full light
	float shadowPower = 1 - Shadow; // 1 is full shadow, useful for scaling effect

	// apply shadows to darkest values (< 1)
	float4 finalColor = saturate(Shadow * (pow(saturate(color), 1 - shadowPower * 0.2))); // boost the gamma of the base image
	finalColor = lerp(luma(finalColor), finalColor, 1 + shadowPower * 0.3); // add some saturation back to the darker parts of the image

	// readd values above 1
	finalColor += max(color - 1, 0.0);
    finalColor.rgb = pows(finalColor.rgb, 1.0/2.2); // delinearise

	return float4(finalColor.rgb, 1);
}

// combine Shadow pass and source using an overlay mode + alpha blending
float4 CombineShadow( VSOUT IN ) : COLOR0 {
	return ApplyInteriorShadow(IN.UVCoord, tex2D(TESR_SourceBuffer, IN.UVCoord), tex2D(TESR_RenderedBuffer, IN.UVCoord).r);
}

technique {

	pass
	{ 
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DepthBlur(TESR_PointShadowBuffer, OffsetMaskH, 1, 3500, MAXDISTANCE);
	}
	
	pass
	{ 
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DepthBlur(TESR_RenderedBuffer, OffsetMaskV, 1, 3500, MAXDISTANCE);
	}
	
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombineShadow();
	}
	
}


// ---- Dedicated path (ShadowsInteriorsEffect::Render) ----
// The blur passes above run on the full-resolution HDR frame through the frame chain: a copy of the
// frame for TESR_SourceBuffer, a clear of an 8 byte per pixel target before each pass, and two blurs
// of what is a single-channel mask. Here the two blurs ping-pong between the two G16R16 scratch
// targets (4 bytes per pixel, no frame copy), and only the last pass touches the HDR frame: it reads
// the untouched scene from TESR_RenderedBuffer and the blurred mask from s4, which the CPU binds.
sampler2D NVR_InteriorShadowIn : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// The depth-aware blur of DepthBlur(), with two differences that do not change the result:
//  * pixels beyond endFade return black -- what clip() left in the cleared destination of the original
//    passes (the shadow is faded out to fully lit by that distance in ApplyInteriorShadow anyway);
//  * the twelve depth reads are skipped when every tap equals the centre, since the weighted average
//    of equal values is that value whatever the weights are.
float4 InteriorShadowBlur(VSOUT IN, uniform sampler2D buffer, uniform float2 OffsetMask, uniform float blurRadius, uniform float depthDrop, uniform float endFade) : COLOR0
{
	float4 center = tex2Dlod(buffer, float4(IN.UVCoord, 0, 0));
	float depth1 = readDepthLod(IN.UVCoord);
	[branch] if (depth1 > endFade) return float4(0, 0, 0, 1);

	float4 taps[cKernelSize];
	float4 spread = 0;
	[unroll]
	for (int t = 0; t < cKernelSize; t++) {
		taps[t] = tex2Dlod(buffer, float4(IN.UVCoord + (BlurOffsets[t] * OffsetMask) * blurRadius, 0, 0));
		spread = max(spread, abs(taps[t] - center));
	}
	[branch] if (max(spread.r, spread.g) < 0.0001f) return float4(center.rgb, 1);

	float WeightSum = 0.114725602f;
	float4 color1 = center * WeightSum;
	depthDrop *= (depth1 / farZ);

	[unroll]
	for (int i = 0; i < cKernelSize; i++)
	{
		float depth2 = readDepthLod(IN.UVCoord + (BlurOffsets[i] * OffsetMask) * blurRadius);
		float diff = abs(float(depth1 - depth2));

		int useForBlur = (diff <= depthDrop);
		color1 += BlurWeights[i] * taps[i] * useForBlur;
		WeightSum += BlurWeights[i] * useForBlur;
	}
	color1 /= WeightSum;
	return float4(color1.rgb, 1);
}

float4 CombineShadowDedicated( VSOUT IN ) : COLOR0 {
	return ApplyInteriorShadow(IN.UVCoord, tex2D(TESR_RenderedBuffer, IN.UVCoord), tex2D(NVR_InteriorShadowIn, IN.UVCoord).r);
}

technique DedicatedInteriorShadows {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 InteriorShadowBlur(NVR_InteriorShadowIn, OffsetMaskH, 1, 3500, MAXDISTANCE);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 InteriorShadowBlur(NVR_InteriorShadowIn, OffsetMaskV, 1, 3500, MAXDISTANCE);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombineShadowDedicated();
	}
}
