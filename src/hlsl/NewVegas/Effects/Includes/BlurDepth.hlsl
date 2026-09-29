#include "Includes/Blur.hlsl"

/*
* A depth aware blur that requires the Depthbuffer and related functions from Depth.hlsl
*/

// perform depth aware 12 taps blur along the direction of the offsetmask
float4 DepthBlur(VSOUT IN, uniform sampler2D buffer, uniform float2 OffsetMask, uniform float blurRadius,uniform float depthDrop,uniform float endFade) : COLOR0
{
	float WeightSum = 0.114725602f;
	float4 color1 = tex2D(buffer, IN.UVCoord) * WeightSum;

	float depth1 = readDepth(IN.UVCoord);
	clip(endFade - depth1);

	depthDrop *= (depth1 / farZ);

	[unroll]
    for (int i = 0; i < cKernelSize; i++)
    {
		float2 uv = IN.UVCoord + (BlurOffsets[i] * OffsetMask) * blurRadius;
		float4 color2 = tex2D(buffer,  uv);
		float depth2 = readDepth(uv);
		float diff = abs(float(depth1 - depth2));

		int useForBlur = (diff <= depthDrop);
		color1 += BlurWeights[i] * color2 * useForBlur;
		WeightSum += BlurWeights[i] * useForBlur;
    }
	color1 /= WeightSum;
    return float4(color1.rgb, 1);
}

// Same blur for a pass whose destination is NOT the texture it samples (ping-pong). Pixels
// beyond endFade are passed through instead of clip()ped: clip leaves the destination holding
// whatever it had before, which equals the input only when rendering in place -- otherwise it is
// stale or uninitialised data that the filtered taps of neighbouring pixels bleed into thin
// geometry against the sky.
float4 DepthBlurKeep(VSOUT IN, uniform sampler2D buffer, uniform float2 OffsetMask, uniform float blurRadius,uniform float depthDrop,uniform float endFade) : COLOR0
{
	float4 center = tex2Dlod(buffer, float4(IN.UVCoord, 0, 0));
	float depth1 = readDepthLod(IN.UVCoord);
	[branch] if (depth1 > endFade) return center;

	// Fetch the neighbours first. When every tap equals the centre -- fully lit or evenly
	// shadowed, which is most of the screen -- the depth-weighted average is the centre value
	// whatever the weights are, so the twelve depth reads can be skipped with identical output.
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
