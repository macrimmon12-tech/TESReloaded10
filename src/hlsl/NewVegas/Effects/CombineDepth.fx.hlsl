float4 TESR_DepthConstants;
float4 TESR_CameraData;

float4x4 TESR_InvProjectionTransform;

sampler2D TESR_DepthBufferWorld : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

static const float viewModelNearZ = TESR_DepthConstants.x;
static const float invertedDepth = TESR_DepthConstants.z;
static const float nearZ = TESR_CameraData.x;
static const float farZ = TESR_CameraData.y;

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

// Convert depth to view space Z.
float ToVS(float depth, float nearZ, float farZ) {
    return nearZ * farZ / (nearZ + depth * (farZ - nearZ));
}

// Convert view space Z to depth.
float ToPS(float viewZ, float nearZ, float farZ){
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}


float4 CombineDepth(VSOUT IN) : COLOR0 {	
	float worldDepth = tex2D(TESR_DepthBufferWorld, IN.UVCoord).x;
	float viewModelDepth = tex2D(TESR_DepthBufferViewModel, IN.UVCoord).x;
	
    float worldViewZ = ToVS(worldDepth, nearZ, farZ);
    float viewModelViewZ = ToVS(viewModelDepth, viewModelNearZ, farZ);
	
    float combinedViewZ = worldViewZ;
    if (viewModelDepth > 0.0)
        combinedViewZ = viewModelViewZ;

    return float4(combinedViewZ / farZ, ToPS(combinedViewZ, nearZ, farZ), 1.0, 1.0);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombineDepth();
	}
}

// ---- Merged depth + normals (two render targets) ----
// Normals.fx reconstructs normals from this pass's output at the pixel and its +-1/+-2 neighbours.
// Every combined-depth texel is a pure function of the raw depth texels at the same position, so
// the same normals can be computed here from the raw buffers (point sampled, same texel centres),
// saving the separate full-screen normals pass. The maths below matches Normals.fx exactly.
float4 TESR_ReciprocalResolution;

float2 CombinedDepthAt(float2 uv)
{
	float worldDepth = tex2D(TESR_DepthBufferWorld, uv).x;
	float viewModelDepth = tex2D(TESR_DepthBufferViewModel, uv).x;
	float combinedViewZ = ToVS(worldDepth, nearZ, farZ);
	if (viewModelDepth > 0.0)
		combinedViewZ = ToVS(viewModelDepth, viewModelNearZ, farZ);
	return float2(combinedViewZ / farZ, ToPS(combinedViewZ, nearZ, farZ));
}

float3 ReconstructPositionFromDepth(float2 uv, float depth)
{
	float4 viewSpace = mul(float4(uv.x * 2 - 1, (1 - uv.y) * 2 - 1, depth, 1), TESR_InvProjectionTransform);
	return viewSpace.xyz / viewSpace.w;
}

struct DepthNormalsOut
{
	float4 depth : COLOR0;
	float4 normal : COLOR1;
};

DepthNormalsOut CombineDepthNormals(VSOUT IN)
{
	float2 uv = IN.UVCoord;

	float4 rightUv = uv.xyxy + float4(1.0, 0.0, 2.0, 0.0) * TESR_ReciprocalResolution.xyxy;
	float4 leftUv = uv.xyxy + float4(-1.0, 0.0, -2.0, 0.0) * TESR_ReciprocalResolution.xyxy;
	float4 bottomUv = uv.xyxy + float4(0.0, 1.0, 0.0, 2.0) * TESR_ReciprocalResolution.xyxy;
	float4 topUv = uv.xyxy + float4(0.0, -1.0, 0.0, -2.0) * TESR_ReciprocalResolution.xyxy;

	float2 centerDepth = CombinedDepthAt(uv);
	float2 rightDepth1 = CombinedDepthAt(rightUv.xy);
	float2 leftDepth1 = CombinedDepthAt(leftUv.xy);
	float2 rightDepth2 = CombinedDepthAt(rightUv.zw);
	float2 leftDepth2 = CombinedDepthAt(leftUv.zw);
	float2 topDepth1 = CombinedDepthAt(topUv.xy);
	float2 bottomDepth1 = CombinedDepthAt(bottomUv.xy);
	float2 topDepth2 = CombinedDepthAt(topUv.zw);
	float2 bottomDepth2 = CombinedDepthAt(bottomUv.zw);

	float depth = centerDepth.x * farZ;
	float4 H = float4(rightDepth1.x, leftDepth1.x, rightDepth2.x, leftDepth2.x) * farZ;
	float4 V = float4(topDepth1.x, bottomDepth1.x, topDepth2.x, bottomDepth2.x) * farZ;

	float2 he = abs((2 * H.xy - H.zw) - depth);
	float2 ve = abs((2 * V.xy - V.zw) - depth);

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

	DepthNormalsOut OUT;
	OUT.depth = float4(centerDepth.x, centerDepth.y, 1.0, 1.0);
	OUT.normal = float4(normalize(cross(vDeriv, hDeriv)) * 0.5 + 0.5, 1.0);
	return OUT;
}

technique DepthNormals
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombineDepthNormals();
	}
}
