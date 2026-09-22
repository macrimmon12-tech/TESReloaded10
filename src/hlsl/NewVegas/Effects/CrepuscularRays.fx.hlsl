// Crepuscular rays (sunbeams through gaps in occluders) for New Vegas Reloaded.
//
// Ported from a from-scratch NVR design (itself descended from arafuse/tes-reloaded's
// OblivionReloaded/Shaders/VolumetricLight/VolumetricLight.fx.hlsl, then rewritten against this
// fork's VSM/EVSM cascade shadow atlas). Replaces GodRays' own "Volumetric" tier and the "Enhanced"
// tier entirely -- both had real, hard-to-fix bugs (see GodRays.fx.hlsl's own history); this design
// fixes the same class of problem a different way and is kept as its own effect/file rather than
// folded into GodRays.fx.hlsl, so it gets its own dedicated half-res render target instead of
// fighting GodRays' existing per-technique register budget and corner-clip buffer sharing.
//
// Two techniques, not two passes of one technique: EffectRecord::Render() keeps a single
// RenderTarget/RenderedSurface pair for every pass of one Render() call, so a low-res march and
// a full-res composite can't share a technique. Technique 0 (March) renders into its own
// dedicated half-res TESR_CrepuscularRaysBuffer via RenderEffectToRT, same pattern as
// FlashlightBeamEffect's TESR_VolumetricBuffer. Technique 1 (Composite) runs later, at full
// resolution, through the normal RenderEffectsPreTonemapping chain.
//
// Design notes carried over from the from-scratch rewrite this was ported from:
//
// A height-bounded "fog layer" march (bounded by a height setting, like GodRays' own since-removed
// Volumetric tier) reads as a flat ground-hugging haze, not shafts -- a fully-lit ray inside that
// volume still glows even with zero occluder in view, so any change to the height band is
// invisible: the wash was never tied to actual occlusion contrast. So: no fog volume, no height
// ceiling. The march is just camera to visible surface (or the falloff cutoff, whichever is
// nearer), testing the sun shadow atlas at each step. The only thing that ever brightens a pixel is
// GetSunShadowAmount() varying along the ray -- i.e. an actual occluder -- and the final output is
// saturated once, at the source, so it can never invert the composite blend.
// General atmospheric haze is VolumetricFog.fx.hlsl's job, not this one's.

float4 TESR_ReciprocalResolution;
float4 TESR_SmoothedSunDir;
float4 TESR_SunColor;
float4 TESR_ShadowFade; // x: sunrise/sunset fade, y: shadow maps active
float4 TESR_FogData; // x: fog near, y: fog far, z: sun glare, w: fog power -- already registered by ShaderManager, shared with VolumetricFog/AmbientOcclusion

float4 TESR_CrepuscularRaysTint;  // xyz: scatter color tint, w: accum distance cutoff (march range)
float4 TESR_CrepuscularRaysData;  // x: strength, y: unused, z: fog influence, w: anisotropy
float4 TESR_CrepuscularRaysDebug; // x: debug view mode, y: dither toggle (z, w unused)

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// POINT, not LINEAR. A depth buffer is not a colour buffer: interpolating between two texels
// either side of a silhouette yields a depth that belongs to neither surface. The march reads
// this at half-res UVs, so every sampled point sits between four full-res texels and every
// silhouette produces a band of rays terminating at an invented mid-air depth -- light with
// nothing to attach to. The composite's depth-aware upsample below wants true neighbour depths
// for the same reason.
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowAtlas : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// Dedicated half-res march result. POINT, because Composite does the reconstruction itself:
// it takes the same four source texels bilinear would have used and weights them by depth
// similarity. Leaving this LINEAR would hand each of those four taps back a pre-blended mix of
// its neighbours, smuggling the cross-silhouette bleed back in underneath the depth weighting.
sampler2D TESR_CrepuscularRaysBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// Blue noise for the march start offset, the same texture SunShadows and AmbientOcclusion use.
// WRAP so it tiles one texel per pixel across the half-res buffer.
sampler2D TESR_NoiseSampler : register(s4) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Shadows.hlsl"

// --- Cascade shadow lookup -----------------------------------------------------------------
// Mirrors SunShadows.fx.hlsl's GetLightAmount (same registered constants, same atlas), copied
// rather than shared: effects compile independently, so this can't be a shared include -- same
// reason SunShadows.fx.hlsl and VolumetricFog.fx.hlsl each already carry their own copy.
// No row_major here: unlike the game-shader path (Shaders/Includes/Shadow.hlsl), which binds
// matrices by raw register index and needs it explicit, D3DX Effects bind by name through
// Effect->SetMatrix() with their own default (non-row_major) packing -- SunShadows.fx.hlsl,
// the other Effect consuming these same matrices, declares them plain for exactly this reason.
// row_major here would silently transpose every light-space transform, sending samples outside
// the valid [0,1] shadow-map region for virtually every real point -- and CLAMP addressing on
// TESR_ShadowAtlas turns "off the edge" into "reads as unoccluded" almost universally.
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformMiddle;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformLod;
float4 TESR_ShadowNearCenter;
float4 TESR_ShadowMiddleCenter;
float4 TESR_ShadowFarCenter;
float4 TESR_ShadowLodCenter;
float4 TESR_ShadowFormatData; // x: mode, y: format bits

static const float ShadowMode = TESR_ShadowFormatData.x;
static const float ShadowFormatBits = TESR_ShadowFormatData.y;

float4 ScreenCoordToTexCoord(float4 coord) {
	coord.xyz /= coord.w;
	coord.x = coord.x * 0.5f + 0.5f;
	coord.y = coord.y * -0.5f + 0.5f;
	return coord;
}

float4 SampleShadowMoments(float2 uv) {
	return tex2Dlod(TESR_ShadowAtlas, float4(uv, 0.0f, 0.0f));
}

// A cascade's shadow map is an orthographic slab, 2*sphereRadius thick, oriented along the sun.
// Outside it there is no occlusion information at all, and the only correct answer is 'lit'.
//
// Without this test the out-of-range coordinate would be handed to the comparison anyway, which
// does not fail gracefully: z > 1 (past the far plane) gives a huge warped depth, so pMax
// collapses to 0 and reads fully SHADOWED, while z < 0 (in front of the near plane) gives a tiny
// one and reads fully LIT -- a hard plane in world space matching no geometry whatever, because
// it is not occlusion.
//
// It would also bury the real signal rather than merely adding to it: every sample that leaves
// the slab returns a hard 0 or 1 regardless of what occluders lie along the ray, and with dozens
// of samples those dominate the average, so the march would report how much of each ray fell
// outside the cascade instead of what crossed it.
//
// SunShadows.fx.hlsl omits this check safely: its receivers are visible surfaces, and the cascade
// was fitted to the view frustum that contains them, so they are inside the slab by construction.
// A ray march walks thousands of units through open air and leaves it constantly.
bool OutsideShadowMap(float3 projected) {
	return max(max(abs(projected.x), abs(projected.y)), abs(projected.z * 2.0f - 1.0f)) > 1.0f;
}

// Light-bleed reduction, adjusted for the atlas format so the effect works on either.
//
// GetEVSMExponents clamps the EVSM positive exponent to 5.54 on a 16-bit atlas (Format = 0)
// against 40 on a 32-bit one. The narrower warp leaves the Chebyshev bound markedly looser, so
// pMax sits higher and occluded points read as partly lit. ReduceLightBleeding removes exactly
// that tail, which makes it the right place to compensate -- rather than requiring the wider
// format, which costs double atlas bandwidth across the whole shadow system.
//
// A ray march is far more exposed to this than surface shading. A shaded surface sits a few
// units behind its occluder, where the bound is still tight; march samples sit hundreds of
// units behind one, where it is weakest. So the shared defaults, tuned for surfaces, are too
// permissive here on 16-bit even though they are fine for the deferred path.
float BleedReduction(float amount) {
	return ShadowFormatBits == 0.0f ? lerp(amount, 0.9f, 0.45f) : amount;
}

// Samples one cascade, or returns -1 when the point does not fall inside that cascade's map.
// The caller uses that to pick a cascade, which is the whole reason this reports it rather
// than clamping: selecting on the projection itself needs no TESR_Shadow*Center constant.
float TryCascade(float4x4 lightTransform, float4 coord, float offsetX, float offsetY, float bias, float bleedReduction) {
	float4 projected = mul(coord, lightTransform);
	projected.xyz /= projected.w;
	if (OutsideShadowMap(projected.xyz)) return -1.0f;

	float4 lightSpaceCoord = float4(projected.x * 0.5f + 0.5f, projected.y * -0.5f + 0.5f, projected.z, 1.0f);
	lightSpaceCoord.xy *= 0.5f;
	lightSpaceCoord.x += offsetX;
	lightSpaceCoord.y += offsetY;

	float4 moments = SampleShadowMoments(lightSpaceCoord.xy);

	[branch]
	if (ShadowMode == 0.0f)
		return GetLightAmountValueVSM(moments.xy, lightSpaceCoord.z, bias, bleedReduction);
	else if (ShadowMode == 1.0f)
		return GetLightAmountValueEVSM2(moments.xy, lightSpaceCoord.z, bias, bleedReduction, ShadowFormatBits);
	else
		return GetLightAmountValueEVSM4(moments, lightSpaceCoord.z, bias, bleedReduction, ShadowFormatBits);
}

float GetSunShadowAmount(float3 positionWS) {
	if (!TESR_ShadowFade.y) return 1.0f;

	const float bias = ShadowMode == 0.0f ? 0.00001f : 0.01f;
	float4 coord = float4(positionWS, 1.0f);

	// Cascade chosen by whether the point projects inside each map, nearest first -- not by
	// distance to TESR_Shadow*Center. A point is in a cascade exactly when it lands inside that
	// cascade's map, which is the same test already needed to reject out-of-range samples above,
	// and matches how the Oblivion source this was originally ported from cascades (GetLightAmount
	// falling through to GetLightAmountFar). It is also strictly more correct than a distance test
	// -- a bounding sphere overlaps the map it approximates rather than matching it -- at the cost
	// of the smooth cross-fade a radius-based blend allows, which is worth losing to get reliable
	// occlusion for a march sampling open air, not just visible surfaces.
	float shadow = TryCascade(TESR_ShadowCameraToLightTransformNear,   coord, 0.0f, 0.0f, bias, BleedReduction(0.1f));
	if (shadow < 0.0f) shadow = TryCascade(TESR_ShadowCameraToLightTransformMiddle, coord, 0.5f, 0.0f, bias, BleedReduction(0.2f));
	if (shadow < 0.0f) shadow = TryCascade(TESR_ShadowCameraToLightTransformFar,    coord, 0.0f, 0.5f, bias, BleedReduction(0.6f));
	if (shadow < 0.0f) shadow = TryCascade(TESR_ShadowCameraToLightTransformLod,    coord, 0.5f, 0.5f, bias, BleedReduction(0.8f));

	// Outside every cascade there is no occlusion information, so lit is the only safe answer.
	if (shadow < 0.0f) return 1.0f;

	return saturate(shadow);
}

// --- Ray march setup -------------------------------------------------------------------------

// Sample count is the resolution at which the march can resolve a shadow volume, and it has to
// beat the occluders you want shafts from. A tree trunk's shadow volume is 25-45 units across
// and a pole's is 15-30; too coarse a step size over the accum-distance range steps clean over
// them, catching them only a fraction of the time and burying that faint dip under the rest of
// the average. Compile-time constant, and an int -- deliberately, not a setting: a
// TOML-driven float loop bound here crashes the D3DX9 HLSL compiler outright (the game dies
// inside CompileEffect with no error logged). ps_3_0's native loop instruction counts with an
// integer register, so a float counter compared against a float bound cannot use it, and fxc
// falls back to a path that does not survive tex2Dlod and nested [branch] blocks in the body.
static const int MARCH_NUM = 64;
static const float NOISE_GRANULARITY = 0.5 / 255.0;

static const float strength = TESR_CrepuscularRaysData.x;
static const float anisotropy = TESR_CrepuscularRaysData.w;
static const float fogInfluence = TESR_CrepuscularRaysData.z;

// Scattering medium density taken from the weather's own fog.
//
// Scattered light is proportional to how much medium the ray crosses, so a constant density
// gives identical shafts in clear desert air and in thick fog, which is wrong in both
// directions: too strong when there is nothing to scatter off, too weak when the air is full
// of it. The game already varies fog per weather, so that is the density to use rather than
// inventing a second one that disagrees with the fog the player can see.
//
// TESR_FogData carries the near and far fog distances. Denser fog reaches full opacity over a
// shorter span, so the span is an inverse density; FOG_REFERENCE_SPAN is the span treated as
// fully dense. Typical clear weather runs tens of thousands of units and lands near 0.1, while
// a fog weather closes to a few thousand and approaches 1.
static const float FOG_REFERENCE_SPAN = 4000.0f;

float GetFogDensity() {
	float span = max(TESR_FogData.y - TESR_FogData.x, 1.0f);
	return saturate(FOG_REFERENCE_SPAN / span);
}

static const bool ditherEnabled = TESR_CrepuscularRaysDebug.y > 0.5f;
// 0 off, 1 the finished march, 2 the raw shadow term along the ray, 3 that same term at the visible surface, 4 the lookup's intermediates, 5-8 the sampling inputs. Mode 2 divides out everything
// layered on top of occlusion -- the phase function, the distance falloff, Strength and
// TESR_SunColor -- and shows only the average of GetSunShadowAmount along each ray. It answers
// the one question the finished output cannot: whether the cascade lookup finds occluders at
// all. A dim frame in mode 1 is ambiguous, because the sun's own colour is near zero shortly
// after sunrise and scales the whole effect with it. Mode 2 depends on no tuning value, no sun
// colour and no time of day: white is lit, black is occluded, and a flat featureless field
// means the lookup returns a constant and no occluder is being detected.
static const float debugMode = TESR_CrepuscularRaysDebug.x;

// A fully-lit ray reading as a flat, undifferentiated plateau regardless of shadow state erases
// whatever contrast the shadow value would otherwise produce. TESR_SunColor carries real HDR
// magnitude in this engine (the same PBR pipeline ObjectTemplate.hlsl's PBRSun/PBRDiffuse
// consume, not a display-range [0,1] color), so a fully-lit ray can clip well before the shadow
// term ever gets a chance to pull it back down. Cut hard so a fully-lit ray has headroom below
// 1.0 for the shadow value to actually carve a visible gap out of, and treat Strength (the
// user-facing setting) as the knob to raise from here, not this.
static const float accumLightStrength = 0.3f;

struct VSOUT {
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN {
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN) {
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

// pow(g, 1.5); fxc does not fold this on its own.
float Pow1_5(float g) {
	return g * sqrt(g);
}

// Henyey-Greenstein phase function, anisotropy clamped to ceiling (wider, flatter lobe for sky
// rays than ground rays -- see the two call sites' scatterCeiling).
//
// Normalised to peak at exactly 1.0 looking straight into the sun, so Anisotropy sets the SHAPE
// of the lobe and nothing else. Raw HG has its magnitude tied to g: the peak is
// (1-g^2)/(4*PI*(1-g)^3), which climbs steeply as g approaches 1 -- so tightening the lobe would
// also make the whole effect dramatically brighter, past where the composite blend saturates,
// washing out to a flat sheet that destroys the very structure the tighter lobe was meant to
// reveal. Dividing through by that peak decouples the two knobs: Anisotropy controls how sharply
// light gathers toward the sun, Strength controls how much there is. The peak term is a closed
// form -- at lightDotView == 1 the denominator is (1 + g^2 - 2g)^1.5 = ((1-g)^2)^1.5 = (1-g)^3 --
// so this costs no extra evaluation.
float ComputeScattering(float lightDotView, float ceiling) {
	float g = min(anisotropy, ceiling);
	float forward = 1.0f - g;
	float denom = 1.0f + g * g - (2.0f * g) * lightDotView;
	return (forward * forward * forward) / Pow1_5(max(denom, 0.0001f));
}

// Low-resolution ray march: walks the view ray from the camera to the visible surface, or to
// the distance-falloff cutoff, whichever is nearer -- sampling the sun shadow atlas at each step. The Henyey-
// Greenstein term itself only depends on view/sun angle, so it's constant along the ray and
// computed once outside the loop; the ONLY thing that varies per step, and the only thing that
// ever brightens a pixel, is the shadow value -- so a fully-lit, unoccluded view (no occluder in
// the ray's path) contributes almost nothing, and contrast only appears where the ray actually
// crosses a shadow boundary.
//
// Ray origin, taken from TESR_InvViewTransform rather than TESR_CameraPosition.
//
// The engine builds the view/inverse-view matrices and TESR_CameraPosition from two different
// camera sources, which can disagree by enough to offset every marched world position -- and
// with it every cascade test and every shadow sample -- while still looking perfectly coherent
// in a frac() grid (debug mode 6 below). The march's direction already comes from toWorld(),
// which is built on TESR_ViewTransform; taking the origin from TESR_InvViewTransform as well
// keeps both from the same source instead of assuming two different constants agree.
float3 GetRayOrigin() {
	return float3(TESR_InvViewTransform[3][0], TESR_InvViewTransform[3][1], TESR_InvViewTransform[3][2]);
}

float4 CrepuscularRaysMarch(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord.xy;
	float3 rayOrigin = GetRayOrigin();

	float depth = readDepth(uv);
	bool isSky = depth > (farZ * 0.99f);

	float3 cameraVector = toWorld(uv) * depth;
	float3 rayDirection = normalize(cameraVector);

	// March only as far as the distance falloff below actually weights: every sample past
	// accumDistance is multiplied by zero, so spending steps out there buys nothing and costs
	// the resolution of the part that does count. Marching to the visible surface (or a long
	// distance into open sky) instead would put a sky ray's step length so wide that only its
	// first two or three samples land inside the falloff window at all, while a ray that hits a
	// wall nearby gets every sample at fine spacing -- that quality cliff between "ray hit
	// something near" and "ray reached sky" reads as soft blobs of light hanging in mid-air,
	// untethered from any geometry.
	float accumDistance = max(TESR_CrepuscularRaysTint.w, 1.0f);
	float rayLength = isSky ? accumDistance : min(length(cameraVector), accumDistance);
	float3 step = rayDirection * (rayLength / MARCH_NUM);

	// Blue noise, not an ordered dither pattern: an ordered pattern repeats every few pixels by
	// construction, so the step boundaries it hides line up into a regular structure instead of
	// scattering -- upsampled from half resolution that shows as clear diagonal banding with a
	// crosshatch texture across every shaft, which a blur cannot remove (blurring bands only
	// produces softer bands). Blue noise has no such periodicity, so the same step boundaries
	// scatter into fine grain a blur genuinely clears. Sampled one texel per pixel, unfiltered,
	// which is how a blue noise mask has to be read to keep its spectral properties.
	float2 noiseUV = uv * (0.5f / TESR_ReciprocalResolution.xy) / 256.0f;
	float ditherOffset = ditherEnabled ? tex2D(TESR_NoiseSampler, noiseUV).r : 0.5f;
	float3 currentPosition = rayOrigin + step * ditherOffset;

	float lightDotView = dot(rayDirection, TESR_SmoothedSunDir.xyz);
	float3 lightColor = TESR_CrepuscularRaysTint.xyz * TESR_SunColor.rgb;
	float scatterCeiling = isSky ? 1.0f : 0.5f;
	// Weather fog scales the medium density. FogInfluence at 0 keeps a constant medium; at 1
	// the shafts track the fog the player can actually see.
	float3 scatterTerm = ComputeScattering(lightDotView, scatterCeiling).xxx * lightColor;
	scatterTerm *= lerp(1.0f, GetFogDensity(), saturate(fogInfluence));

	float3 accumLight = 0.0f.xxx;
	float accumShadow = 0.0f;

	[loop]
	for (int i = 0; i < MARCH_NUM; i++) {
		// 1.0 where this step sees the sun, towards 0 behind an occluder.
		float Shadow = GetSunShadowAmount(currentPosition);
		accumShadow += Shadow;

		// Per-step distance falloff, not a single value based on the ray's endpoint: a step near
		// the camera should contribute the same whether the ray eventually hits a nearby wall or
		// continues on to open sky far beyond it. Keying the falloff to the ray's endpoint
		// distance instead would make the effect only ever visible painted onto whatever solid
		// surface a given ray happened to hit -- a sky-bound ray's endpoint is always the far cap
		// and gets crushed to ~0, while a nearby object's endpoint is always close and gets the
		// "full strength" falloff uniformly across its whole silhouette -- never a glow genuinely
		// hanging in open air.
		float distFalloff = 1.0f - saturate(distance(currentPosition, rayOrigin) / accumDistance);

		accumLight += scatterTerm * Shadow * distFalloff;
		currentPosition += step;
	}

	// Modes 5-8: the INPUTS to the sampling, not its result. Everything above debugs the
	// shadow lookup while assuming what is fed to it is correct. These check that assumption,
	// because a wrong input produces a plausible-looking wrong output and cannot be told apart
	// from a wrong lookup by staring at the finished image.
	//
	// 5 DEPTH. readDepth(uv) over accumDistance, greyscale. Expect a smooth near-black to white
	//   gradient with crisp object silhouettes. Flat, banded or inverted means the depth buffer
	//   or farZ is wrong, and then every world position built from it is wrong too.
	[branch] if (debugMode > 4.5f && debugMode < 5.5f)
		return float4(saturate(readDepth(uv) / accumDistance).xxx, 1.0f);

	// 6 WORLD POSITION. frac(pos / 512) as RGB: a world-aligned grid repeating every 512 units.
	//   Expect coloured banding that stays welded to surfaces as the camera moves, and stays put
	//   when only the camera rotates. Swimming with rotation means the reconstruction is wrong.
	[branch] if (debugMode > 5.5f && debugMode < 6.5f)
		return float4(frac((rayOrigin + cameraVector) / 512.0f), 1.0f);

	// 7 CASCADE SELECTION for the surface point. red near, green middle, blue far, yellow lod,
	//   BLACK none. GetSunShadowAmount starts at shadow = 1.0 and only assigns if a cascade
	//   claims the point, so anything black here is returning lit without sampling the atlas at
	//   all. Expect concentric bands, red nearest the camera.
	[branch] if (debugMode > 6.5f && debugMode < 7.5f) {
		float3 p = rayOrigin + cameraVector;
		if (length(p - TESR_ShadowNearCenter.xyz)   < TESR_ShadowNearCenter.w)   return float4(1,0,0,1);
		if (length(p - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) return float4(0,1,0,1);
		if (length(p - TESR_ShadowFarCenter.xyz)    < TESR_ShadowFarCenter.w)    return float4(0,0,1,1);
		if (length(p - TESR_ShadowLodCenter.xyz)    < TESR_ShadowLodCenter.w)    return float4(1,1,0,1);
		return float4(0,0,0,1);
	}

	// 8 GATING CONSTANTS, as one flat colour. red TESR_ShadowFade.y (0 disables the lookup
	//   entirely and returns 1.0 before anything is sampled), green shadow mode over 2 so VSM,
	//   EVSM2 and EVSM4 read as 0, 0.5 and 1, blue the format bit where 0 is the 16-bit atlas
	//   whose clamped EVSM exponent costs the depth precision. Any red channel at zero means the
	//   effect cannot produce shadow at all and nothing downstream is worth reading.
	[branch] if (debugMode > 7.5f && debugMode < 8.5f)
		return float4(TESR_ShadowFade.y, ShadowMode * 0.5f, ShadowFormatBits, 1.0f);

	// 9 CASCADE RADII, as one flat colour: TESR_Shadow{Near,Middle,Far}Center.w over 250,
	//   1000 and 3000. Solid black here means not one of the four cascade tests passes anywhere
	//   on screen -- and those tests read these constants directly, without touching the atlas.
	//   Black means the constants are arriving as zero, a binding failure rather than anything
	//   in the shader maths.
	[branch] if (debugMode > 8.5f && debugMode < 9.5f)
		return float4(saturate(TESR_ShadowNearCenter.w / 250.0f),
		              saturate(TESR_ShadowMiddleCenter.w / 1000.0f),
		              saturate(TESR_ShadowFarCenter.w / 3000.0f), 1.0f);

	// 10 DISTANCE from the surface point to the near cascade centre, over 2000. A smooth
	//   gradient means the centre is a real world position near the camera. Uniform white means
	//   it is far away or at the origin, which is what a zeroed constant looks like.
	[branch] if (debugMode > 9.5f && debugMode < 10.5f)
		return float4(saturate(length((rayOrigin + cameraVector) - TESR_ShadowNearCenter.xyz) / 2000.0f).xxx, 1.0f);

	// 11 CASCADE SELECTION AT A MARCH SAMPLE -- the midpoint of the ray, not the visible
	//   surface. Mode 7 and mode 10 both use the uncapped surface position, and for a sky pixel
	//   that can be hundreds of thousands of units out, correctly outside every cascade -- black
	//   there is consistent with the lookup working. March samples are capped at accumDistance
	//   from the camera, so unlike the surface they should land inside a cascade; anything black
	//   here is a real failure. red near, green middle, blue far, yellow lod, black none.
	[branch] if (debugMode > 10.5f && debugMode < 11.5f) {
		float3 m = rayOrigin + rayDirection * (rayLength * 0.5f);
		if (length(m - TESR_ShadowNearCenter.xyz)   < TESR_ShadowNearCenter.w)   return float4(1,0,0,1);
		if (length(m - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) return float4(0,1,0,1);
		if (length(m - TESR_ShadowFarCenter.xyz)    < TESR_ShadowFarCenter.w)    return float4(0,0,1,1);
		if (length(m - TESR_ShadowLodCenter.xyz)    < TESR_ShadowLodCenter.w)    return float4(1,1,0,1);
		return float4(0,0,0,1);
	}

	// 12 CAMERA SOURCE DISAGREEMENT, as a flat grey: the distance between TESR_CameraPosition
	//   and the origin encoded in TESR_InvViewTransform, over 64 units. Black means the two
	//   agree; anything brighter is the offset that would otherwise be applied to every world
	//   position the march builds if GetRayOrigin() used TESR_CameraPosition instead.
	[branch] if (debugMode > 11.5f)
		return float4(saturate(length(TESR_CameraPosition.xyz - GetRayOrigin()) / 64.0f).xxx, 1.0f);

	// Mode 4: the lookup's intermediates for the surface point, rather than its verdict. Shows
	// where each sample actually lands and what it reads there:
	//   RED, GREEN = the near cascade's light-space UV, scaled so the quadrant fills 0..1.
	//                Expect a smooth gradient across the frame. Flat or pinned to an edge means
	//                the transform is wrong and CLAMP is returning the atlas border everywhere.
	//   BLUE       = the moment actually sampled there, scaled by the EVSM positive exponent so
	//                it is visible. Bright blue means a cleared far texel, which reads as lit;
	//                varying blue means real occluder depths are being read and the fault is in
	//                the comparison rather than the addressing.
	[branch] if (debugMode > 3.5f) {
		float3 surfacePos = rayOrigin + cameraVector;
		// The cascade the real lookup would select, not a hardcoded one -- pinning this to Near
		// would make the mode useless: Near spans a couple hundred units, so nearly every visible
		// point projects outside it, saturate clamps to 0 or 1, and the result is flat colour-cube
		// corners that look like a broken transform while being entirely correct.
		float4x4 sel = TESR_ShadowCameraToLightTransformLod;
		if (length(surfacePos - TESR_ShadowNearCenter.xyz)   < TESR_ShadowNearCenter.w)   sel = TESR_ShadowCameraToLightTransformNear;
		else if (length(surfacePos - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) sel = TESR_ShadowCameraToLightTransformMiddle;
		else if (length(surfacePos - TESR_ShadowFarCenter.xyz)    < TESR_ShadowFarCenter.w)    sel = TESR_ShadowCameraToLightTransformFar;
		float4 lsc = ScreenCoordToTexCoord(mul(float4(surfacePos, 1.0f), sel));
		float2 atlasUV = lsc.xy * 0.5f;
		float moment = SampleShadowMoments(atlasUV).x;
		return float4(saturate(lsc.x), saturate(lsc.y), saturate(moment / exp(5.54f)), 1.0f);
	}

	// Mode 3: the same lookup run at the VISIBLE SURFACE instead of the air samples -- exactly
	// the position SunShadows.fx.hlsl feeds it, which is known to shadow correctly. This is the
	// A/B that says whether GetSunShadowAmount is broken outright or only for free-floating
	// points. If mode 3 shows proper shadows and mode 2 does not, the lookup is sound and the
	// fault is specific to air samples -- most likely their light-space depth falling outside the
	// cascade's near/far range, which reads as a cleared (far) texel and so as lit.
	[branch] if (debugMode > 2.5f) {
		float3 surfacePos = rayOrigin + cameraVector;
		return float4(GetSunShadowAmount(surfacePos).xxx, 1.0f);
	}

	// Shadow term on its own, before anything is layered over it -- see debugMode above.
	[branch] if (debugMode > 1.5f) return float4((accumShadow / MARCH_NUM).xxx, 1.0f);

	// Mean sample value, then back to a path integral: the physical quantity is the integral of
	// scattered light along the ray, sum(f) * stepLength, and stepLength is rayLength/MARCH_NUM.
	// Dividing by the sample count alone yields a mean with NO dependence on how far the ray
	// travelled, so a few units of air in front of a near wall would accumulate exactly as much
	// light as thousands of units of open sky -- every surface in the frame would get the same
	// wash regardless of how much air was really in front of it, reading as haze paint on nearby
	// geometry rather than depth. Normalised by accumDistance so a full-length ray keeps the
	// magnitude this was calibrated at and Strength stays meaningful.
	accumLight *= rayLength / (accumDistance * MARCH_NUM);
	accumLight *= accumLightStrength * strength;
	accumLight += lerp(-NOISE_GRANULARITY, NOISE_GRANULARITY, rand(uv));

	// Saturated here, once, at the source: the composite's blend (color*(1-v)+v) only behaves
	// as a blend for v in [0,1] -- above that it inverts and swamps the scene color entirely.
	return float4(saturate(accumLight), 1.0f);
}

// One tap of the depth-aware upsample. exp2 falls off fast enough that a tap on the far side
// of a silhouette contributes essentially nothing, while the depth difference across a
// continuous surface (even a steeply raked one) stays well inside the kernel. Relative to
// centerDepth, not absolute: the same slope spans a far larger absolute depth range far away
// than up close, and an absolute tolerance would either bleed up close or over-reject far off.
void AccumulateTap(float2 tapUV, float centerDepth, inout float3 sum, inout float weightSum) {
	float tapDepth = readDepth(tapUV);
	float weight = exp2(-32.0f * abs(tapDepth - centerDepth) / max(centerDepth, 1.0f));
	sum += tex2D(TESR_CrepuscularRaysBuffer, tapUV).rgb * weight;
	weightSum += weight;
}

// Full resolution: upsamples the low-res march and blends it onto the scene. No separate
// sky-ambient term: this stays a pure shadow-occlusion shaft effect, not a general atmospheric
// glow -- the march itself already scales every contribution by GetSunShadowAmount, so a pixel
// only lights up here where an occluder actually created contrast against the sun.
//
// The upsample is depth-aware rather than a plain bilinear tex2D. A half-res texel straddling a
// silhouette mixes a short ray (near surface, little accumulated light) with a long one (open sky
// behind it, much more), and bilinear then smears that mixture several full-res pixels to either
// side of the edge -- a halo of light detached from the object that should bound it.
float4 CrepuscularRaysComposite(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord.xy;

	// TESR_ReciprocalResolution is the full-res texel; the half-res texel is twice that, so this
	// offset is exactly half a half-res texel -- the four source texels bilinear would have used.
	float2 offset = TESR_ReciprocalResolution.xy;
	float centerDepth = readDepth(uv);

	float3 sum = 0.0f.xxx;
	float weightSum = 0.0f;
	AccumulateTap(uv + float2(-offset.x, -offset.y), centerDepth, sum, weightSum);
	AccumulateTap(uv + float2( offset.x, -offset.y), centerDepth, sum, weightSum);
	AccumulateTap(uv + float2(-offset.x,  offset.y), centerDepth, sum, weightSum);
	AccumulateTap(uv + float2( offset.x,  offset.y), centerDepth, sum, weightSum);

	// When every tap is rejected -- a thin feature, or a strong depth discontinuity where all
	// four neighbours sit on the far side of an edge -- dividing by the epsilon floor returns
	// black, not a neutral result, painting a single-pixel black speckle along every silhouette
	// in the frame. Falling back to the nearest single tap keeps the pixel's own value instead
	// of inventing a black one: if no neighbour shares this pixel's depth, the unfiltered sample
	// is exactly what should be used.
	float3 volumeLight = weightSum < 0.0001f
		? tex2D(TESR_CrepuscularRaysBuffer, uv).rgb
		: sum / weightSum;

	// Debug view: the march's own output with no scene under it, so what the effect actually
	// computes can be read directly instead of inferred from how it tints the frame. Blown-out
	// highlights and a correctly shaped but over-bright shaft look identical once blended.
	if (debugMode > 0.5f) return float4(volumeLight, 1.0f);

	float3 color = linearize(tex2D(TESR_SourceBuffer, uv)).rgb;
	volumeLight = linearize(volumeLight);
	float3 result = color * (1 - volumeLight) + volumeLight;
	return delinearize(float4(result, 1.0f));
}

technique March {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CrepuscularRaysMarch();
	}
}

technique Composite {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CrepuscularRaysComposite();
	}
}
