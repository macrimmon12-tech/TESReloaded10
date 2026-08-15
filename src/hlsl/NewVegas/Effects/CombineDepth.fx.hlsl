string PipelinePosition = "PreTonemapping";

// Utility shader that merges the world and view-model (weapon) depth buffers
// into a single combined depth texture consumed by other effects. No settings
// of its own.
float4 TESR_DepthConstants
<
	string name = "Depth Constants";
	string description = "Packed depth-buffer metrics supplied by the engine: x = view-model (weapon) near Z, y = far Z (unused, always 0), z = 1.0 if the depth buffer is reversed else 0.0, w = 1.0 (unused). Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_CameraData
<
	string name = "Camera Data";
	string description = "Packed camera metrics supplied by the engine: x = near Z, y = far Z, z = frustum aspect ratio (width/height), w = field of view. Not user-configurable.";
	float defaultValue = 0.0;
>;

float4x4 TESR_InvProjectionTransform
<
	string name = "Inverse Projection Transform";
	string description = "Inverse of the current camera projection matrix, supplied by the engine each frame for view-space position reconstruction from depth. Not user-configurable.";
	float defaultValue = 0.0;
>;

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
