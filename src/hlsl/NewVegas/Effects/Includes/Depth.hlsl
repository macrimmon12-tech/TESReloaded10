// A collection of functions that allow to query depth of a given pixel and also to reconstruct/project a point from screen to view space
// requires the shader to get access to the TESR_DepthBuffer sampler before the include.

// Shared, engine-global uniforms (declared once here and pulled by textual
// #include into every effect that needs depth reconstruction -- annotated at
// this single declaration site rather than duplicated per including .fx.hlsl
// file, per R3e). None are user-configurable.
float4x4 TESR_ProjectionTransform
<
	string widget = "hidden";
	string name = "Projection Transform";
	string description = "Current camera projection matrix, supplied by the engine each frame. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4x4 TESR_InvProjectionTransform
<
	string widget = "hidden";
	string name = "Inverse Projection Transform";
	string description = "Inverse of the current camera projection matrix, supplied by the engine each frame for view-space position reconstruction from depth. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4x4 TESR_ViewTransform
<
	string widget = "hidden";
	string name = "View Transform";
	string description = "Current camera view matrix, supplied by the engine each frame. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4x4 TESR_InvViewTransform
<
	string widget = "hidden";
	string name = "Inverse View Transform";
	string description = "Inverse of the current camera view matrix, supplied by the engine each frame for world-space position reconstruction from depth. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_DepthConstants
<
	string widget = "hidden";
	string name = "Depth Constants";
	string description = "Packed depth-buffer metrics supplied by the engine: x = view-model (weapon) near Z, y = far Z (unused, always 0), z = 1.0 if the depth buffer is reversed else 0.0, w = 1.0 (unused). Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_CameraData
<
	string widget = "hidden";
	string name = "Camera Data";
	string description = "Packed camera metrics supplied by the engine: x = near Z, y = far Z, z = frustum aspect ratio (width/height), w = field of view. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_CameraPosition
<
	string widget = "hidden";
	string name = "Camera Position";
	string description = "World-space camera position, supplied by the engine each frame. Not user-configurable.";
	float defaultValue = 0.0;
>;

static const float invertedDepth = TESR_DepthConstants.z;
static const float nearZ = TESR_CameraData.x;
static const float farZ = TESR_CameraData.y;
static const float Q = farZ/(farZ - nearZ);

float readDepth(float2 coord)
{
	return tex2D(TESR_DepthBuffer, coord).x * farZ;
}

float3 reconstructPosition(float2 uv)
{
    float x = uv.x * 2 - 1;
    float y = (1 - uv.y) * 2 - 1;
    float z = tex2D(TESR_DepthBuffer, uv).y;
    float4 clipSpace = float4(x, y, z, 1.0f);

    float4 viewSpace = mul(clipSpace, TESR_InvProjectionTransform);
	
    viewSpace /= viewSpace.w;
	
    return viewSpace.xyz;
}

float3 projectPosition(float3 position){
	float4 projection = mul(float4 (position, 1.0), TESR_ProjectionTransform);
	projection.xyz /= projection.w;

	projection.x = projection.x * 0.5 + 0.5;
	projection.y = 0.5 - 0.5 * projection.y;

	return projection.xyz;
}


float3 toWorld(float2 tex)
{
	float3 v = float3(TESR_ViewTransform[0][2], TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]);
	v += (1 / TESR_ProjectionTransform[0][0] * (2 * tex.x - 1)).xxx * float3(TESR_ViewTransform[0][0], TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]);
	v += (-1 / TESR_ProjectionTransform[1][1] * (2 * tex.y - 1)).xxx * float3(TESR_ViewTransform[0][1], TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]);
	return v;
}

float getHomogenousDepth(float2 uv){
	float depth = readDepth(uv);
	float3 camera_vector = toWorld(uv) * depth;
	return length(camera_vector);
}

float4 reconstructWorldPosition(float2 uv, out float viewDepth){
    float x = uv.x * 2 - 1;
    float y = (1 - uv.y) * 2 - 1;
    float z = tex2D(TESR_DepthBuffer, uv).y;
    float4 clipSpace = float4(x, y, z, 1.0f);

    float4 viewSpace = mul(clipSpace, TESR_InvProjectionTransform);
	
    viewSpace /= viewSpace.w;
	
    viewDepth = viewSpace.z;
	
    float4 worldSpace = mul(viewSpace, TESR_InvViewTransform);

    return float4(worldSpace.xyz, 1.0f);
}