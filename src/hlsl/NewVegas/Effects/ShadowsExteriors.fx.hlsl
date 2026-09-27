// Image space shadows shader for Oblivion Reloaded
# define viewshadows 0

#ifndef FORWARD_SHADOWS
	#define FORWARD_SHADOWS 0
#endif

float4 TESR_ReciprocalResolution;
float4 TESR_WaterSettings; //x: water height in the cell, y: water depth darkness, z: is camera underwater
float4 TESR_ShadowData; // x: quality, y: darkness, z: nearmap resolution, w: farmap resolution
float4 TESR_ShadowFade; // x: fading at sunrise/sunset, y:disabled shadows, z: pointlights shadows
float4 TESR_ShadowForwardData; // x: 1 when the forward path is suppressed, so the cascades arrive here
float4 TESR_SkyColor;
float4 TESR_SunAmbient;
float4 TESR_SunColor;
float4 TESR_SunDirection;
float4 TESR_ShadowScreenSpaceData;
float4 TESR_ShadowComposite; // x: composite mode, y: normal distrust, z: 1 while the PBR shaders are enabled, w: 1 while TESR_ShadowForwardBuffer holds this frame's cascade term
float4 TESR_PBRData; // z: LightingScale, w: AmbientScale, as the PBR object shaders apply them
float4 TESR_PBRExtraData; // y: SkylightingScale
float4 TESR_SkyIrradiance[9]; // order-2 SH sky irradiance from Sky.cpp; [0].w is 1 once it has been computed
float4 TESR_AmbientOcclusionAOData; // z: ClampStrength
float4 TESR_AmbientOcclusionData; // y: LumThreshold
float4 TESR_AmbientOcclusionFold; // x: 1 when this pass applies the ambient occlusion in TESR_AmbientOcclusionBuffer

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = ANISOTROPIC; MIPFILTER = LINEAR; };
sampler2D TESR_PointShadowBuffer : register(s2)  = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// Last: EffectRecord binds the Nth declared sampler to slot N. x: the cascade term the forward
// pass built this frame (ForwardTemporalShadow in SunShadows.fx.hlsl), read at this pixel.
sampler2D TESR_ShadowForwardBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// The occlusion AmbientOcclusion.fx rendered ahead of this pass (its Compute technique), read at this pixel.
sampler2D TESR_AmbientOcclusionBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };


static const float DARKNESS = max(0.0,1-TESR_ShadowData.y);

struct VSOUT
{
	float4 vertPos : POSITION;
	float4 normal : TEXCOORD1;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/Shadows.hlsl"


VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

/*
 * The previous composite, unchanged: darkens the pixel by the shadow amount whichever way the
 * surface faces, then blends towards the sky colour so that darkening does not read as grey.
 * Composite mode 1, on either path.
 */
float4 LegacyComposite(float4 color, float2 Shadow)
{
	// scale shadows strength to ambient before adding attenuation for pointlights (ShadowFade.z means point Lights are on)
	float ambient = lerp(1, luma(TESR_SunAmbient), DARKNESS * TESR_ShadowFade.z); // linearise
	Shadow.r = lerp(0, ambient, Shadow.r); //scale brightest areas to the ambient so it can be lit further with attenuation
	Shadow.r += Shadow.g; // Apply poing light attenuation (includes point light shadows)

	Shadow.r = lerp(DARKNESS, 1.0, Shadow.r); 	// brighten shadow value from 0 to darkness from config value

	Shadow.r = saturate(Shadow.r);

#if viewshadows == 1
	return float4(Shadow.rrr, 1.0f);
#endif
    color.rgb = pows(color.rgb, 2.2); // linearise
    float4 skyColor = float4(pows(TESR_SkyColor.rgb, 2.2),TESR_SkyColor.w); // linearise
	// tint shadowed areas with Sky color before blending
	float4 colorShadow = luma(color.rgb) * Shadow.r * skyColor;
	colorShadow.rgb = lerp(colorShadow, color * Shadow.r, saturate(Shadow.r + 0.5)).rgb;// bias the transition between the 2 colors to make it less noticeable
    colorShadow.rgb = pows(max(0.0,colorShadow.rgb), 1.0/2.2); // delinearise
	return float4(colorShadow.rgb, 1.0);
}

/*
 * The ambient the PBR object shaders lit this pixel with: the weather ambient, redistributed by
 * orientation using the sky's spherical harmonics. Mirrors SkyAmbientRedistribute in
 * Shaders/Includes/SkyAmbient.hlsl (its spherical-harmonic mode), duplicated because the effect
 * and game shader trees compile independently - change one, change the other.
 *
 * trust stands in for the object shaders' valid flag: where the normal cannot be read, the
 * orientation-free average is used, the same fallback they take where the world position is
 * undefined. The object shaders' single-sample skylight mode (SkylightingMode 1) is not mirrored;
 * under it this estimate is the harmonic one.
 */
float3 RedistributedAmbient(float3 flatAmbient, float3 n, float trust, float strength)
{
	const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

	float3 skyLin = TESR_SkyIrradiance[0].rgb
		+ TESR_SkyIrradiance[1].rgb * n.y
		+ TESR_SkyIrradiance[2].rgb * n.z
		+ TESR_SkyIrradiance[3].rgb * n.x
		+ TESR_SkyIrradiance[4].rgb * (n.x * n.y)
		+ TESR_SkyIrradiance[5].rgb * (n.y * n.z)
		+ TESR_SkyIrradiance[6].rgb * (3.0f * n.z * n.z - 1.0f)
		+ TESR_SkyIrradiance[7].rgb * (n.x * n.z)
		+ TESR_SkyIrradiance[8].rgb * (n.x * n.x - n.y * n.y);

	float3 groundLin = flatAmbient * flatAmbient;
	float3 meanLin = TESR_SkyIrradiance[0].rgb + 0.5f * groundLin;
	float3 dirLin = lerp(meanLin, skyLin + groundLin * (0.5f - 0.5f * n.z), trust);

	float rescale = dot(groundLin, lumaWeights) / max(dot(meanLin, lumaWeights), 1e-6f);
	return lerp(flatAmbient, sqrt(max(dirLin * rescale, 0.0f)), saturate(strength) * saturate(TESR_SkyIrradiance[0].w));
}

/*
 * Load Shadows Buffer and filter water surfaces
 * returns a shadow value from darkness setting value (full shadow) to 1 (full light)
*/
float4 ShadowComposite(VSOUT IN)
{
	float4 color = tex2D(TESR_RenderedBuffer, IN.UVCoord);
	float2 uv = IN.UVCoord;

	float depth = readDepth(uv);
	float3 camera_vector = toWorld(uv) * depth;
	float4 world_pos = float4(TESR_CameraPosition.xyz + camera_vector, 1.0f);
	float3 world_normal = GetWorldNormal(IN.UVCoord);

	// early out for underwater surface (if camera is underwater and surface to shade is close to water level with normal pointing downward)
	if (TESR_WaterSettings.z == 1 && world_pos.z < (TESR_WaterSettings.x + 2) && world_pos.z > (TESR_WaterSettings.x - 2) && dot(world_normal, float3(0, 0, -1)) > 0.999) return color;

	float2 Shadow = tex2D(TESR_PointShadowBuffer, IN.UVCoord).rg;
	Shadow.r = lerp(TESR_ShadowFade.x, 1.0f, Shadow.r); // fade shadows to light when sun is low

	// Read at top level: a gradient taking sample is illegal under the branches below (X3528).
	float forwardCascades = tex2D(TESR_ShadowForwardBuffer, IN.UVCoord).x;

	[branch]
	if (TESR_ShadowComposite.x == 1.0f) return LegacyComposite(color, Shadow);

	// The sun visibility the frame already carries, and the one it should end with. On the deferred
	// path this pass owns all of it: the frame carries none, and Shadow.r is the cascades and the
	// contact shadows together. While the forward path runs, the object shaders have already taken
	// the cascades off the sun term, and only the contact shadows and the point lights arrive here.
	// Applied as the frame's whole visibility they would take the sun away from cascade-shadowed
	// pixels a second time, so they are combined with the cascades the way the deferred path
	// combines them - the darker of the two - and the ratio then removes only the difference. The
	// forward pass leaves the cascade term it built this frame in TESR_ShadowForwardBuffer; while
	// it does not run, no contact shadows are drawn either, and taking the cascades as full light
	// leaves the ratio at 1.
	//
	// The previous forward treatment was the whole-pixel darkening of mode 1, which dims ambient
	// and all, on faces turned away from the sun too. A constant that fails to arrive reads zero,
	// which lands on the forward branch.
	bool deferred = true;
#if FORWARD_SHADOWS
	deferred = TESR_ShadowForwardData.x > 0.0f;
#endif
	float carried = 1.0f;
	float target = Shadow.r;
	[branch]
	if (!deferred) {
		carried = lerp(TESR_ShadowFade.x, 1.0f, TESR_ShadowComposite.w > 0.0f ? forwardCascades : 1.0f);
		target = min(Shadow.r, carried);
	}

	// Point lights light a sun shadow back up.
	carried = saturate(carried + Shadow.g * TESR_ShadowFade.z);
	target = saturate(target + Shadow.g * TESR_ShadowFade.z);

	// What the surface would be lit by with the sun taken away, over what it is lit by now.
	//
	//     lit      = albedo * (sun * saturate(N.L) + ambient)
	//     shadowed = albedo * ambient
	//     shadowed / lit = ambient / (ambient + sun * saturate(N.L))
	//
	// Albedo cancels, so a shadowed pixel can be made from a lit one by multiplication alone - no
	// G buffer, and nothing the object shaders need to know about. TESR_SunColor and
	// TESR_SunAmbient are WorldSky's sunDirectional and sunAmbient, the pair the engine's own
	// lighting sums. With the PBR shaders enabled they are scaled here the way those shaders scale
	// them (LightingScale, AmbientScale) and the ambient takes the same sky redistribution, so the
	// two terms describe what the pixel was lit with; PBR's diffuse term is albedo * N.L * light,
	// so the sun term compares directly. Terrain has scales of its own that this pass cannot tell
	// apart from an object's, so the PBR ones are used for everything.
	//
	// Two things fall out of this that the flat darkening had to fake. A surface facing away from
	// the sun is left alone, because it was never in sunlight and removing the sun from it changes
	// nothing. And shadowed surfaces end up the colour of the ambient by construction, which is
	// what the sky tint in LegacyComposite approximates.
	//
	// The terms are used as they arrive rather than linearised first: the frame this pass reads and
	// the constants share one encoding, so a pow on only one side of the comparison would pull the
	// two apart.
	//
	// The normal is sampled half a texel off centre. Normals.fx reconstructs it from depth, and
	// alpha tested foliage under DXVK's coverage dither writes a depth checkerboard, so over grass
	// the reconstruction alternates per pixel. Half a texel off centre, the bilinear filter averages
	// an exact two by two block, which holds two of each phase, so the pattern cancels outright. A
	// four tap cross would not: the four neighbours of a checkerboard cell are all the opposite
	// phase.
	float2 normalUv = uv + 0.5f * TESR_ReciprocalResolution.xy;
	float3 ratioNormal = GetWorldNormal(normalUv);

	// Normals.fx publishes how planar the depth around a pixel was, and over grass the answer is
	// not at all: there is no single surface for a normal to describe. Where the normal cannot be
	// read the shadow keeps its strength and gives up only the orientation term - N.L falls back to
	// 1 and the ambient to its orientation-free average - which is the part that was never knowable
	// there.
	float confidence = tex2D(TESR_NormalsBuffer, normalUv).a;
	float trust = saturate(1.0f - (1.0f - confidence) * TESR_ShadowComposite.y);

	// Mode 2 forces N.L to 1, the one input the ratio takes that the flat darkening did not. An
	// artefact that survives it is not coming from the normal.
	float NdotL = lerp(1.0f, saturate(dot(ratioNormal, TESR_SunDirection.xyz)), trust);
	if (TESR_ShadowComposite.x == 2.0f) NdotL = 1.0f;

	float pbr = TESR_ShadowComposite.z;
	float3 sunLight = TESR_SunColor.rgb * NdotL * lerp(1.0f, TESR_PBRData.z, pbr);
	float3 ambientFlat = TESR_SunAmbient.rgb * lerp(1.0f, TESR_PBRData.w, pbr);
	float3 ambient = RedistributedAmbient(ambientFlat, ratioNormal, trust, TESR_PBRExtraData.y * pbr);

	// DARKNESS is 1 minus the Darkness setting. At Darkness 1 the sun is fully removed where the
	// shadow says it should be, which is the physical result, and anything lower lets some of it
	// back through. There is deliberately no way to go darker: the sun is already entirely gone.
	// Deferred path only. The object shaders take the whole sun off the cascades they carry, so on
	// the forward path the contact shadows combined with those here take all of it too.
	float vis = lerp(target, 1.0f, deferred ? DARKNESS : 0.0f);
	float3 shading = (sunLight * vis + ambient) / max(sunLight * carried + ambient, 0.0001f);

	// Views, so an artefact can be attributed to this pass or ruled out of it without guessing
	// from the composited result.
	[branch]
	if (TESR_ShadowComposite.x == 3.0f) return float4(shading, 1.0f);
	[branch]
	if (TESR_ShadowComposite.x == 4.0f) return float4(ratioNormal * 0.5f + 0.5f, 1.0f);
	[branch]
	if (TESR_ShadowComposite.x == 5.0f) return float4(world_normal * 0.5f + 0.5f, 1.0f);
	[branch]
	if (TESR_ShadowComposite.x == 6.0f) return float4(trust.xxx, 1.0f);
	[branch]
	if (TESR_ShadowComposite.x == 7.0f) return float4(ambient, 1.0f);

#if viewshadows == 1
	return float4(shading, 1.0f);
#endif
	return float4(color.rgb * shading, 1.0f);
}

/*
 * The shadow composite, then the ambient occlusion AmbientOcclusion.fx would otherwise multiply the
 * frame by in a pass of its own right after this one - its Combine, step for step, applied to
 * every result the composite returns.
 */
float4 Shadow(VSOUT IN) : COLOR0
{
	float4 color = ShadowComposite(IN);

	[branch]
	if (TESR_AmbientOcclusionFold.x > 0.0f) {
		float3 lin = pows(color.rgb, 2.2);
		float ao = lerp(TESR_AmbientOcclusionAOData.z, 1.0, tex2Dlod(TESR_AmbientOcclusionBuffer, float4(IN.UVCoord, 0.0f, 0.0f)).r);
		ao = lerp(ao, 1.0, saturate((luma(lin) - TESR_AmbientOcclusionData.y) * 3.0));
		color = float4(pows(lin * ao, 1.0 / 2.2), 1.0f);
	}
	return color;
}


technique {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow();
	}
}
