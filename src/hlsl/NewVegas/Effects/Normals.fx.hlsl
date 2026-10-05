float4 TESR_ReciprocalResolution;

sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };


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

#include "Includes/Depth.hlsl"
#include "Includes/Helpers.hlsl"

// (linear depth, post-projection depth) for a texel; the second is recomputed from the first
// because the depth buffer may be a single channel (see projectedDepthFromLinear).
float2 DepthPair(float2 uv)
{
	float linear01 = tex2D(TESR_DepthBuffer, uv).x;
	return float2(linear01, projectedDepthFromLinear(linear01));
}

float3 ReconstructPositionFromDepth(float2 uv, float depth)
{
	float4 viewSpace = mul(float4(uv.x * 2 - 1, (1 - uv.y) * 2 - 1, depth, 1), TESR_InvProjectionTransform);
	return viewSpace.xyz / viewSpace.w;
}

float4 ComputeNormals(VSOUT IN) :COLOR0
{
	float2 uv = IN.UVCoord;

	// improved normal reconstruction algorithm from 
	// https://gist.github.com/bgolus/a07ed65602c009d5e2f753826e8078a0

	// store coordinates at 1 and 2 pixels from center in all directions
	float4 rightUv = uv.xyxy + float4(1.0, 0.0, 2.0, 0.0) * TESR_ReciprocalResolution.xyxy; 
	float4 leftUv = uv.xyxy + float4(-1.0, 0.0, -2.0, 0.0) * TESR_ReciprocalResolution.xyxy; 
	float4 bottomUv = uv.xyxy + float4(0.0, 1.0, 0.0, 2.0) * TESR_ReciprocalResolution.xyxy; 
	float4 topUv =uv.xyxy + float4(0.0, -1.0, 0.0, -2.0) * TESR_ReciprocalResolution.xyxy; 

	// Each sample contains linear depth in x and device depth in y. Reuse the same nine
	// reads for edge selection and position reconstruction; the old shader fetched the five
	// reconstruction samples a second time.
	float2 centerDepth = DepthPair(uv);
	float2 rightDepth1 = DepthPair(rightUv.xy);
	float2 leftDepth1 = DepthPair(leftUv.xy);
	float2 rightDepth2 = DepthPair(rightUv.zw);
	float2 leftDepth2 = DepthPair(leftUv.zw);
	float2 topDepth1 = DepthPair(topUv.xy);
	float2 bottomDepth1 = DepthPair(bottomUv.xy);
	float2 topDepth2 = DepthPair(topUv.zw);
	float2 bottomDepth2 = DepthPair(bottomUv.zw);

	float depth = centerDepth.x * farZ;
	float4 H = float4(rightDepth1.x, leftDepth1.x, rightDepth2.x, leftDepth2.x) * farZ;
	float4 V = float4(topDepth1.x, bottomDepth1.x, topDepth2.x, bottomDepth2.x) * farZ;

	float2 he = abs((2 * H.xy - H.zw) - depth);
	float2 ve = abs((2 * V.xy - V.zw) - depth);

	// pick horizontal and vertical diff with the smallest depth difference from slopes
	float3 centerPoint = ReconstructPositionFromDepth(uv, centerDepth.y);
	float3 rightPoint = ReconstructPositionFromDepth(rightUv.xy, rightDepth1.y);
	float3 leftPoint = ReconstructPositionFromDepth(leftUv.xy, leftDepth1.y);
	float3 topPoint = ReconstructPositionFromDepth(topUv.xy, topDepth1.y);
	float3 bottomPoint = ReconstructPositionFromDepth(bottomUv.xy, bottomDepth1.y);
	float3 left = centerPoint - leftPoint;
	float3 right = rightPoint - centerPoint;
	float3 down = centerPoint - bottomPoint;
	float3 up = topPoint - centerPoint;

	float3 hDeriv = he.x > he.y ? left : right;
	float3 vDeriv = ve.x > ve.y ? down : up;

	// get view space normal from the cross product of the best derivatives
	// half3 viewNormal = normalize(cross(hDeriv, vDeriv));
	float3 viewNormal = normalize(cross(vDeriv, hDeriv));

	return float4 (compress(viewNormal), 1.0);
}


technique
{
	pass
	{ 
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 ComputeNormals();
	}
}
