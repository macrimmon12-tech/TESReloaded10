// RainMotion: real world-space rain-streak geometry for New Vegas Reloaded.
//
// Deliberately independent of Precipitations.fx.hlsl (the existing screen-space raymarched
// rain volume) -- no shared constants, no shared technique. Each streak is a camera-facing
// billboard quad whose *only* per-vertex input is a local corner (x: -1/1 width side, y: 0/1
// leading/trailing edge) and its own instance index; every other property of the streak
// (its placement, size and length variance) is derived purely by hashing that index, so there
// is no per-instance CPU data and no ceiling on streak count from constant-register space.
//
// Placement uses a camera-following, toroidally wrapped volume (TESR_RainMotionVolume):
// each streak's hashed local offset is folded back into a box re-centered on the *live*
// TESR_CameraPosition every frame, so the rain can never be "left behind" no matter how far
// the player walks -- there is no fixed origin or bounded extent to walk out of.

float4x4 TESR_ViewProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformOrtho;
float4 TESR_RainMotionData;    // x: intensity, y: effective fall speed, z: streak length, w: streak width
float4 TESR_RainMotionFall;    // xyz: normalized fall vector (world space), w: signed camera-whip shear
float4 TESR_RainMotionVolume;  // xyz: wrap volume size (Sx, Sy, Sz), w: streak count (informational)
float4 TESR_RainMotionFade;    // x: fade start (fraction of half-extent), y: fade range, z: refraction strength, w: opacity
float4 TESR_CameraForward;
float4 TESR_GameTime;
float4 TESR_SunColor;

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_OrthoMapBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Depth.hlsl"

// TEMPORARY DIAGNOSTIC SWITCH -- set to 0 to restore normal behavior. While 1, both discard
// tests (roof/ortho occlusion, depth soft-particle) are skipped and every streak is forced to
// an opaque bright magenta quad, so we can tell whether geometry is reaching the screen at all
// versus being culled by one of those tests.
#define RAINMOTION_DEBUG_FORCE_VISIBLE 1

float hash11(float n) { return frac(sin(n) * 43758.5453123f); }
float3 hash3(float n) { return float3(hash11(n), hash11(n + 17.17f), hash11(n + 41.41f)); }

struct VSOUT
{
	float4 vertPos   : POSITION;
	float2 uv        : TEXCOORD0; // x: width param -1..1, y: length param 0 (leading) .. 1 (trailing)
	float4 orthoPos  : TEXCOORD1; // for the roof/indoor occlusion test
	float  viewDepth : TEXCOORD2; // for the soft-particle scene-depth test
	float  fade      : TEXCOORD3; // per-instance wrap-edge fade factor
	float4 screenPos : TEXCOORD4; // clip position, to derive screen UV in the pixel shader
};

// corner.x: -1/1 width side, corner.y: 0/1 leading/trailing edge, corner.z: instance index
VSOUT RainMotionVS(float3 corner : POSITION0)
{
	VSOUT OUT = (VSOUT)0.0f;

	float cornerX = corner.x;
	float cornerY = corner.y;
	float instanceIndex = corner.z;

	float3 volume = TESR_RainMotionVolume.xyz;
	float3 h = hash3(instanceIndex);
	float3 localOffset = (h - 0.5f) * volume;

	float lengthVariant = 0.6f + 0.8f * hash11(instanceIndex + 91.7f);
	float widthVariant  = 0.7f + 0.6f * hash11(instanceIndex + 133.3f);
	float fallPhase     = hash11(instanceIndex + 7.77f) * volume.z;

	float3 fallVector = TESR_RainMotionFall.xyz; // already normalized CPU-side
	float3 fallAccum = fallVector * TESR_RainMotionData.y * TESR_GameTime.z;

	// toroidal wrap: fold the hashed local offset (plus how far it has "fallen") back into
	// a box centered on the origin, then recenter that box on the live camera position below.
	float3 wrapped;
	wrapped.x = (frac((localOffset.x + fallAccum.x) / volume.x + 0.5f) - 0.5f) * volume.x;
	wrapped.y = (frac((localOffset.y + fallAccum.y) / volume.y + 0.5f) - 0.5f) * volume.y;
	wrapped.z = (frac((localOffset.z + fallAccum.z + fallPhase) / volume.z + 0.5f) - 0.5f) * volume.z;

	float3 streakCenter = TESR_CameraPosition.xyz + wrapped;

	// axis-aligned billboard: width axis stays perpendicular to both the fall direction and
	// the direction to the camera, so the streak presents its flat face to the viewer while
	// still leaning with the fall vector -- a real rotation of real geometry, so perspective
	// and parallax fall out of the ordinary transform below for free.
	float3 toCam = TESR_CameraPosition.xyz - streakCenter;
	float toCamLen = max(length(toCam), 0.0001f);
	toCam /= toCamLen;

	float3 widthAxis = cross(fallVector, toCam);
	float widthAxisLen = length(widthAxis);
	widthAxis = (widthAxisLen > 0.0001f) ? (widthAxis / widthAxisLen) : float3(1.0f, 0.0f, 0.0f);

	float streakLength = TESR_RainMotionData.z * lengthVariant;
	float streakWidth  = TESR_RainMotionData.w * widthVariant;
	float smear        = TESR_RainMotionFall.w; // signed camera-whip shear

	float3 lengthOffset = -fallVector * (cornerY * streakLength);
	float3 widthOffset  = widthAxis * (cornerX * 0.5f * streakWidth);
	float3 shearOffset  = widthAxis * (smear * cornerY); // shear grows toward the trailing edge

	float3 worldPos = streakCenter + widthOffset + lengthOffset + shearOffset;

#if RAINMOTION_DEBUG_FORCE_VISIBLE
	// Bypass ONLY the wrap/billboard math: use the real camera matrices, but place each
	// streak at a trivial, hand-picked point (spread out a little by instance index so
	// they're not all exactly coincident) a fixed, modest distance in front of the camera,
	// along the camera's forward axis. Isolates whether TESR_CameraPosition/
	// TESR_ViewProjectionTransform are valid for this shader at all, separate from the
	// wrap/billboard computation above.
	float3 spread = float3(frac(instanceIndex * 0.0173f) * 400.0f - 200.0f,
	                        frac(instanceIndex * 0.0313f) * 400.0f - 200.0f,
	                        0.0f);
	worldPos = TESR_CameraPosition.xyz + normalize(TESR_CameraForward.xyz) * 500.0f + spread;
#endif
	float4 clipPos = mul(float4(worldPos, 1.0f), TESR_ViewProjectionTransform);
	OUT.vertPos = clipPos;
	OUT.screenPos = clipPos;
	OUT.uv = float2(cornerX, cornerY);
	OUT.viewDepth = clipPos.w;

	OUT.orthoPos = mul(float4(worldPos, 1.0f), TESR_ShadowCameraToLightTransformOrtho);

	float3 edgeDist = abs(wrapped) / (volume * 0.5f); // 0 at the recenter origin, 1 at the wrap boundary
	float maxEdge = max(edgeDist.x, max(edgeDist.y, edgeDist.z));
	OUT.fade = 1.0f - saturate((maxEdge - TESR_RainMotionFade.x) / max(TESR_RainMotionFade.y, 0.0001f));

	return OUT;
}

float4 RainMotionPS(VSOUT IN) : COLOR0
{
	float shapeWidth = 1.0f - smoothstep(0.0f, 1.0f, abs(IN.uv.x));
	float shapeLength = 1.0f - IN.uv.y;
	float shape = shapeWidth * shapeLength * IN.fade;
#if !RAINMOTION_DEBUG_FORCE_VISIBLE
	if (shape <= 0.001f) discard;
#endif

	float2 screenUV;
	screenUV.x = IN.screenPos.x / IN.screenPos.w * 0.5f + 0.5f;
	screenUV.y = 0.5f - (IN.screenPos.y / IN.screenPos.w * 0.5f);

#if RAINMOTION_DEBUG_FORCE_VISIBLE
	return float4(1.0f, 0.0f, 1.0f, 1.0f); // opaque magenta -- if you see this, geometry is reaching the screen
#endif

	// roof/indoor occlusion, reusing the same top-down exposure buffer other precipitation-
	// adjacent effects already rely on for this exact test.
	float3 orthoPos = IN.orthoPos.xyz / IN.orthoPos.w;
	bool outOfBounds = (orthoPos.x < -1.0f || orthoPos.x > 1.0f || orthoPos.y < -1.0f || orthoPos.y > 1.0f || orthoPos.z < 0.0f || orthoPos.z > 1.0f);
	float2 orthoUV = float2(orthoPos.x * 0.5f + 0.5f, orthoPos.y * -0.5f + 0.5f);
	float orthoDepth = tex2D(TESR_OrthoMapBuffer, orthoUV).r;
	bool occluded = outOfBounds || (orthoDepth < orthoPos.z - 0.0001f);
	if (occluded) discard;

	// soft-particle fade against opaque scene geometry, using the same combined depth buffer
	// every other post effect reads rather than a hardware Z-test (no depth-stencil is bound
	// at this stage of the post-process pipeline).
	float sceneDepth = readDepth(screenUV);
	float depthFade = saturate((sceneDepth - IN.viewDepth) / 50.0f);
	shape *= depthFade;
	if (shape <= 0.001f) discard;

	// per-streak refraction: bend the background behind the drop rather than tint over it.
	float2 refractNormal = float2(IN.uv.x, 0.0f);
	float2 refractUV = screenUV + refractNormal * TESR_RainMotionFade.z * 0.01f;
	float3 sceneColor = tex2D(TESR_SourceBuffer, refractUV).rgb;

	float glint = pow(saturate(1.0f - abs(IN.uv.x)), 8.0f) * shapeLength;
	float3 color = sceneColor + TESR_SunColor.rgb * glint * 0.35f;

	float alpha = saturate(shape * TESR_RainMotionData.x * TESR_RainMotionFade.w);
	return float4(color, alpha);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 RainMotionVS();
		PixelShader = compile ps_3_0 RainMotionPS();
		ZEnable = false;
		ZWriteEnable = false;
		AlphaBlendEnable = true;
		SrcBlend = SRCALPHA;
		DestBlend = INVSRCALPHA;
		CullMode = NONE;
		AlphaTestEnable = false;
	}
}
