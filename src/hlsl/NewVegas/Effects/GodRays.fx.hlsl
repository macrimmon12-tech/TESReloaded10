// GodRays full screen shader for Oblivion/Skyrim Reloaded

float4 TESR_ReciprocalResolution;
float4 TESR_GameTime;
float4 TESR_SunColor;
float4 TESR_GodRaysRay; // x: intensity, y:length, z: density, w: visibility -- Classic only
float4 TESR_GodRaysRayColor; // x:r, y:g, z:b, w:saturate
float4 TESR_GodRaysData; // x: passes amount, y: luminance, z:multiplier, w: time enabled
// Enhanced-only tuning. Kept separate from TESR_GodRaysRay rather than sharing its slots: Enhanced's
// raymarch parameters (a per-step decay factor, a step spacing) don't share sensible value ranges
// with Classic's RayLength/RayDensity (a blur step multiplier, an unused legacy slot), so reusing
// those would make tuning one technique fight the other.
float4 TESR_GodRaysEnhanced; // x: RayDecay, y: RayStepScale, z: BlurStrength, w: GlareStrength
float4 TESR_ViewSpaceLightDir; // view space light vector
float4 TESR_SunDirection; // worldspace sun light vector
float4 TESR_SunPosition; // worldspace sundisk position
float4 TESR_ShadowFade; // attenuation factor of sunsets/sunrises and moon phases
float4 TESR_SunAmount;
float4 TESR_SunsetColor;
float4 TESR_DebugVar;

// Volumetric-only tuning. Two vectors since Ray/RayColor/Data/Enhanced are all already full, and
// Volumetric's parameters (raymarch step count, fade distances, a fog-layer height band, a
// shadowed-point falloff) don't share sensible ranges with any of those either.
float4 TESR_GodRaysVolumetric1; // x: Steps, y: MaxDistance, z: HeightCutoff, w: LayerThickness
float4 TESR_GodRaysVolumetric2; // x: ShadowedCutoffDistance, y: NearWeightFalloff, z: Strength, w: unused

// Sun shadow cascade data, duplicated from VolumetricFog.fx.hlsl (same globals ShadowsExteriorEffect
// registers, same GetFogShadowVisibility) -- separate effects compile independently, so this can't be
// shared directly between the two files, same reason SunShadows.fx.hlsl's own copy exists.
// Only Near+Far here, not all 4 cascades VolumetricFog.fx.hlsl tests: GodRays.fx.hlsl pools Classic +
// Enhanced + Volumetric's globals into one shared ps_3_0 224-register budget (D3DX Effects allocate a
// register per declared global for the whole file, not per technique), and the full 4-cascade copy
// pushed this file over that limit (error X4507). Middle/Lod dropped rather than Near/Far to match
// the actual Oblivion Reloaded reference implementation of this technique, which also only tests 2
// cascades (GetLightAmount/GetLightAmountFar). Safe because cascades nest (Near subset of Middle
// subset of Far subset of Lod, each a larger radius): dropping Middle costs precision, not coverage,
// in the range it used to serve, since Far's larger radius already covers it. The one real gap is
// samples beyond Far's radius (near VolumetricMaxDistance's outer edge) now falling back to "always
// lit" instead of Lod's coverage -- acceptable since VolumetricRaymarch's own NearWeightFalloff
// already de-emphasizes those far samples' contribution to the accumulated light.
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4 TESR_ShadowNearCenter;   // xyz: center (world space), w: radius
float4 TESR_ShadowFarCenter;
float4 TESR_ShadowFormatData; // x: mode (0 VSM, 1 EVSM2, 2 EVSM4), y: format bits per pixel
float4 TESR_ShadowBlur;       // x: 1 / atlas resolution
float4 TESR_SmoothedSunDir;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_AvgLumaBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowAtlas : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Shadows.hlsl"
#include "Includes/Sky.hlsl"
#include "Includes/Normals.hlsl"

static const float raspect = 1.0f / TESR_ReciprocalResolution.z;
static const float samples = 10;
static const float stepLength = 1/samples;
static const float scale = 0.5;
static const float4x4 ditherMat = {{0.0588, 0.5294, 0.1765, 0.6471},
									{0.7647, 0.2941, 0.8824, 0.4118},
									{0.2353, 0.7059, 0.1176, 0.5882},
									{0.9412, 0.4706, 0.8235, 0.3259}};

static const float lumTreshold = TESR_GodRaysData.y;
static const float multiplier = TESR_GodRaysData.z;
static const float intensity = TESR_GodRaysRay.x;
static const float stepLengthMult = TESR_GodRaysRay.y;
static const float glareReduction = TESR_GodRaysRay.z;
static const float godrayCurve = TESR_GodRaysRay.w;
static const float sunHeight = 1 - shade(TESR_SunPosition.xyz, blue.xyz);

// Enhanced technique tuning -- step count reuses TESR_GodRaysData.x (same field Classic leaves
// unused; see its TOML doc), everything else comes from the dedicated TESR_GodRaysEnhanced vector.
static const int GodRaysPasses = max(1, int(TESR_GodRaysData.x));
static const float RayDecay = saturate(TESR_GodRaysEnhanced.x);
static const float RayStepScale = max(0, TESR_GodRaysEnhanced.y);
static const float BlurStrength = max(0, TESR_GodRaysEnhanced.z);
static const float GlareStrength = max(0, TESR_GodRaysEnhanced.w);

// Volumetric technique tuning -- see VolumetricRaymarch/VolumetricCombine below.
static const int VolumetricSteps = clamp(int(TESR_GodRaysVolumetric1.x), 1, 32); // sane upper bound on a dynamic [loop] trip count, not an unroll limit
static const float VolumetricMaxDistance = max(1, TESR_GodRaysVolumetric1.y);
static const float VolumetricHeightCutoff = TESR_GodRaysVolumetric1.z;
static const float VolumetricLayerThickness = max(1, TESR_GodRaysVolumetric1.w);
static const float VolumetricShadowedCutoffDistance = max(0, TESR_GodRaysVolumetric2.x);
static const float VolumetricNearWeightFalloff = saturate(TESR_GodRaysVolumetric2.y);
static const float VolumetricStrength = max(0, TESR_GodRaysVolumetric2.z);

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

float4 SkyMask(VSOUT IN) : COLOR0 {
	
	float2 uv = IN.UVCoord / scale;
	clip((uv <= 1) - 1);

	float sunset = pows(sunHeight, 8);
    float3 sunColor = linearize(TESR_SunColor).rgb + lerp(linearize(TESR_SunsetColor.rgb), 0, sunset); // linearise

	float glarePower = lerp(0.1, 8.0, sunset); // increase flare boost during sunrise/sunset

	float depth = (readDepth(uv) / farZ) > 0.9; //only pixels belonging to the sky will register
	float3 sunGlare = pows(dot(TESR_ViewSpaceLightDir.xyz, normalize(reconstructPosition(uv))), 180) * glarePower; // fake sunglare computed from light direction
	float3 color = linearize(tex2D(TESR_SourceBuffer, uv)).rgb;
	color = (color + sunGlare * sunColor) * depth * smoothstep(0, 0.01, sunHeight);

	return float4(color, 1.0f);
}


float4 LightMask(VSOUT IN) : COLOR0 {
	// isolates the brightest parts of the sky to only use those for radial blur
	
	float2 uv = IN.UVCoord;
	clip((uv <= scale) - 1);

	// quick average lum with 4 samples at corner pixels
	float3 color = tex2D(TESR_RenderedBuffer, uv).rgb;
	color += tex2D(TESR_RenderedBuffer, uv + float2(-1, -1) * TESR_ReciprocalResolution.xy).rgb;
	color += tex2D(TESR_RenderedBuffer, uv + float2(-1, 1) * TESR_ReciprocalResolution.xy).rgb;
	color += tex2D(TESR_RenderedBuffer, uv + float2(1, -1) * TESR_ReciprocalResolution.xy).rgb;
	color += tex2D(TESR_RenderedBuffer, uv + float2(1, 1) * TESR_ReciprocalResolution.xy).rgb;
	color /= 5;

	// extract bright pixels
	float treshold = lerp(2.0, 0.0, pow(abs(sunHeight), 8)); // scale the bloom power with sunsets/sunrises
	float bloom = smoothstep(treshold, treshold + lumTreshold * 15, luma(color));

	color = saturate(bloom * color * 100 * intensity);

	return float4(color.rgb, 1.0f);
}


float4 RadialBlur(VSOUT IN, uniform float step) : COLOR0 {
	float2 uv = IN.UVCoord;
	clip((uv <= scale) - 1);
	uv /= scale; // restore uv scale to do calculations in [0, 1] space
	uv -= 0.5 * TESR_ReciprocalResolution.xy;

	// calculate vector from pixel to sun along which we'll sample
	float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy;

	// vector from the given pixel to the sun position
	float2 blurDirection = (sunPos.xy - uv) * float2(1.0f, raspect); // apply aspect ratio correction
	float distance = length(blurDirection); // distance from pixel to radial blur center

	float2 dir = blurDirection/distance;

	float stepSize = step * stepLengthMult;
	float maxStep = distance/stepSize;

	// sample the light clamped image from the pixel to the sun for the given amount of samples
	float2 samplePos = uv;
	float4 color = float4(0, 0, 0, 1);
	float total = 1;
	for (float i=0; i < samples; i++){
		float length = min(stepSize * i, distance); // clamp sampling vector to the distance from the pixel to the sun
		samplePos = saturate(uv + (dir * length / float2(1, raspect))); // apply aspect ratio correction

		float doStep = (i <= maxStep && samplePos.x > 0 && samplePos.y > 0 && samplePos.x < 1 && samplePos.y < 1); // check if we haven't overshot the sun position or exited the screen
		color += tex2D(TESR_RenderedBuffer, samplePos * scale) * doStep;
		total += doStep;
	}
	color /= total;

	return float4(color.rgb, 1);
}


float4 Combine(VSOUT IN) : COLOR0
{
	float scale = 0.5; // godrays were rendered at smaller res
	float4 color = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	float2 uv = IN.UVCoord;
	float3 eyeDir = normalize(reconstructPosition(uv));
	
	// calculate vector from pixel to sun to get the distance
	float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy;
	float2 blurDirection = (sunPos.xy - uv) * float2(1.0f, raspect); // apply aspect ratio correction
	float distance = length(blurDirection);

	uv *= scale;
	float4 rays = tex2D(TESR_RenderedBuffer, uv);

	// attentuate intensity with distance from sun to fade the edges and reduce sunglare
	float heightAttenuation = TESR_GodRaysData.w?lerp(0.2, 4.0, pows(sunHeight, 4)):1.0; // if timeEnabled is on, godrays strength is reduced when the sun is high
	float glareAttenuation = 1.0;
	// float glareAttenuation = smoothstep(0, glareReduction, distance);
	float attenuation = pow(compress(shade(TESR_ViewSpaceLightDir.xyz, eyeDir)), 2.5) * glareAttenuation * heightAttenuation * (sunHeight < 1);

	// calculate sun color
    float3 sunColor = GetSunColor(shade(TESR_SunDirection.xyz, blue.xyz), 1, TESR_SunAmount.x, TESR_SunColor.rgb, TESR_SunsetColor.rgb);
    float3 godRayColor = linearize(TESR_GodRaysRayColor).rgb;

	//rays = pows(rays, godrayCurve); // increase response curve to extract more definition from godray pass
	rays.rgb *= multiplier * lerp(sunColor, godRayColor, TESR_GodRaysRayColor.w);
	rays.rgb *= attenuation;

	// reduce banding by dithering areas impacted by the rays
	//float maxDitherLuma = 0.05; // 0.2 ^ 2.2, rounded down
	//bool useDither = (rays.r + rays.g + rays.b > 0) && (pows(tex2D(TESR_AvgLumaBuffer, float2(0.5, 0.5)),2.2).x < maxDitherLuma); // only dither when there is some ray & when average luma is low
	//uv /= TESR_ReciprocalResolution.xy;
	//rays.rgb += (ditherMat[(uv.x)%4 ][ (uv.y)%4 ] / 255) * useDither;

	color += max(rays, 0) * 5 * color + max(rays, 0) * 0.2;
	color = delinearize(color);
	return float4(color.rgb, 1);
}


// ================= Enhanced: decayed-raymarch shaft accumulation =================
// Adapted from an older NVR-era GodRays shader (real Kenny Mitchell/GPU Gems 3 "Volumetric Light
// Scattering": per-step exponential illumination decay, not an averaged blur), reusing this file's
// existing sun-screen-projection idiom (TESR_ViewSpaceLightDir + projectPosition, same as Classic's
// RadialBlur/Combine above) instead of the old shader's own manual view/projection matrix math.

float4 RayMaskEnhanced(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord / scale;
	clip((uv <= 1) - 1);

	// Graduated by raw depth rather than a hard sky-only cutoff, so near-horizon terrain silhouettes
	// contribute proportionally to the seed instead of an all-or-nothing sky mask.
	float depth = readDepth(uv) / farZ;
	float3 color = linearize(tex2D(TESR_SourceBuffer, uv)).rgb;
	return float4(color * depth, 1.0f);
}

float4 LightShaftEnhanced(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	clip((uv <= scale) - 1);
	uv /= scale;

	float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy;
	float2 blurDirection = (sunPos.xy - uv) * float2(1.0f, raspect);
	float distanceToSun = length(blurDirection);
	float2 dir = blurDirection / max(distanceToSun, 0.0001);

	float stepSize = min(0.3, distanceToSun) * RayStepScale / GodRaysPasses;
	float2 samplePos = uv;
	float3 color = tex2D(TESR_RenderedBuffer, uv * scale).rgb;
	float illuminationDecay = 1.0;

	[unroll(64)]
	for (int i = 0; i < GodRaysPasses; i++) {
		samplePos -= (dir * stepSize) / float2(1.0f, raspect); // undo aspect correction to step in real UV space
		float3 s = tex2D(TESR_RenderedBuffer, saturate(samplePos) * scale).rgb;
		color += s * illuminationDecay;
		illuminationDecay *= RayDecay;
	}
	color *= intensity / GodRaysPasses;

	return float4(color, 1.0f);
}

float4 BlurEnhanced(VSOUT IN) : COLOR0 {
	// Tangential (perpendicular-to-sun-direction) blur to hide the raymarch's step banding.
	float2 uv = IN.UVCoord;
	clip((uv <= scale) - 1);

	float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy * scale;
	float2 radial = normalize(uv - sunPos);
	// True perpendicular (-y, x) of radial, each component scaled by its own axis' texel size --
	// the previous float2(Recip.y, -Recip.x) pairing had the axes crossed and the sign on the wrong
	// term, which is neither a correct 90-degree rotation nor correctly aspect-scaled; at most
	// on-screen angles it blurred close to radially instead of tangentially, compounding the
	// existing radial accumulation into streaky garbage instead of smoothing it.
	float2 tangent = float2(-radial.y * TESR_ReciprocalResolution.x, radial.x * TESR_ReciprocalResolution.y) * BlurStrength;

	float4 col = tex2D(TESR_RenderedBuffer, uv);
	col += 0.67f * tex2D(TESR_RenderedBuffer, uv + tangent);
	col += 0.67f * tex2D(TESR_RenderedBuffer, uv - tangent);
	col += 0.33f * tex2D(TESR_RenderedBuffer, uv + 2.0f * tangent);
	col += 0.33f * tex2D(TESR_RenderedBuffer, uv - 2.0f * tangent);

	return float4(col.rgb * 0.333f, 1.0f);
}

float3 BlendSoftLight(float3 a, float3 b) {
	float3 c = 2.0f * a * b * (1.0f + a * (1.0f - b));
	float3 a_sqrt = sqrt(a);
	float3 d = (a + b * (a_sqrt - a)) * 2.0f - a_sqrt;
	return (b < 0.5f) ? c : d;
}

float4 CombineEnhanced(VSOUT IN) : COLOR0 {
	float4 ori = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	float2 uv = IN.UVCoord * scale;
	float4 rays = tex2D(TESR_RenderedBuffer, uv);

	float3 eyeDir = normalize(reconstructPosition(IN.UVCoord));
	float heightAttenuation = TESR_GodRaysData.w ? lerp(0.2, 4.0, pows(sunHeight, 4)) : 1.0;
	float attenuation = pow(compress(shade(TESR_ViewSpaceLightDir.xyz, eyeDir)), 2.5) * heightAttenuation * (sunHeight < 1);

	float3 sunColor = GetSunColor(shade(TESR_SunDirection.xyz, blue.xyz), 1, TESR_SunAmount.x, TESR_SunColor.rgb, TESR_SunsetColor.rgb);
	float3 godRayColor = linearize(TESR_GodRaysRayColor).rgb;
	float3 rayTint = lerp(sunColor, godRayColor, TESR_GodRaysRayColor.w);

	// Darkness-weighted: rays read weaker over already-bright pixels, stronger over dark/shadowed
	// ones, instead of a flat additive boost -- avoids blowing out highlights the way Classic's
	// `color += rays * 5 * color + rays * 0.2` can. Soft-light composite instead of a plain add.
	rays.rgb *= multiplier * rayTint * attenuation * saturate(1.0 - ori.rgb);

	float4 color = ori + rays;

	// BlendSoftLight assumes [0,1] inputs (sqrt(a) and the (1-b) term are only meaningful in that
	// range); ori+rays is linear HDR and routinely exceeds 1 (bright sky/sun pixels alone, before
	// rays even add anything), which was feeding the blend out-of-domain and producing broken/
	// garbled output rather than a smooth graded highlight. Soft-light-shade only the [0,1] base and
	// re-add whatever was above 1 afterward, so HDR highlights that should still bloom downstream
	// aren't silently clipped away by the blend.
	float3 hdrOverflow = max(0, color.rgb - 1.0);
	color.rgb = BlendSoftLight(saturate(color.rgb), saturate(rayTint * multiplier + 0.5f)) + hdrOverflow;
	color.rgb = delinearize(color.rgb);
	return float4(color.rgb, 1.0f);
}


// ================= Volumetric: real shadow-raymarched shafts + glare =================
// Adapted from VolumetricFog.fx.hlsl's own GetFogShadowVisibility (same globals ShadowsExteriorEffect
// registers, same VSM/EVSM-filtered GetFogShadowValue), but trimmed to 2 cascades (Near/Far) instead
// of fog's own 4 (Near/Middle/Far/Lod) -- see the TESR_ShadowCameraToLightTransformNear/Far comment
// above for why (a ps_3_0 224-register budget this file was pushed over). This also happens to match
// what the actual Oblivion Reloaded reference implementation of this technique uses. Separate effects
// compile independently, so this is a second copy of this shadow lookup, not a shared include -- same
// reason SunShadows.fx.hlsl and VolumetricFog.fx.hlsl each carry their own copy already.

float4 ScreenCoordToTexCoord(float4 coord) {
	coord.xyz /= coord.w;
	coord.x = coord.x * 0.5f + 0.5f;
	coord.y = coord.y * -0.5f + 0.5f;
	return coord;
}

float GetFogShadowValue(float4x4 lightTransform, float4 coord, float offsetX, float offsetY, float bias) {
	float4 LightSpaceCoord = ScreenCoordToTexCoord(mul(coord, lightTransform));
	LightSpaceCoord.xy *= 0.5;
	LightSpaceCoord.x += offsetX;
	LightSpaceCoord.y += offsetY;

	float4 moments = tex2Dlod(TESR_ShadowAtlas, float4(LightSpaceCoord.xy, 0.0f, 0.0f));

	float Mode = TESR_ShadowFormatData.x;
	float FormatBits = TESR_ShadowFormatData.y;

	[branch]
	if (Mode == 0.0f)
		return GetLightAmountValueVSM(moments.xy, LightSpaceCoord.z, bias, 0.2f);
	else if (Mode == 1.0f)
		return GetLightAmountValueEVSM2(moments.xy, LightSpaceCoord.z, bias, 0.2f, FormatBits);
	else
		return GetLightAmountValueEVSM4(moments, LightSpaceCoord.z, bias, 0.2f, FormatBits);
}

float GetFogShadowVisibility(float4 positionWS, float3 normal) {
	float NdotL = dot(normal, TESR_SmoothedSunDir.xyz);
	float offsetScale = saturate(1 - NdotL);

	float2 radii = { TESR_ShadowNearCenter.w, TESR_ShadowFarCenter.w };
	float2 texelWorld = 4.0f * radii * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
	float2 offsetDistance = offsetScale * 2.5f * texelWorld;

	float bias = (TESR_ShadowFormatData.x == 0.0f ? 0.00001f : 0.01f) * (1.0f + offsetScale);
	const float blend = 0.9f;

	float2 shadows = {
		GetFogShadowValue(TESR_ShadowCameraToLightTransformNear, float4(positionWS.xyz + offsetDistance.x * normal, 1.0f), 0.0, 0.0, bias),
		GetFogShadowValue(TESR_ShadowCameraToLightTransformFar,  float4(positionWS.xyz + offsetDistance.y * normal, 1.0f), 0.0, 0.5, bias),
	};

	float2 distances = {
		length(positionWS.xyz - TESR_ShadowNearCenter.xyz),
		length(positionWS.xyz - TESR_ShadowFarCenter.xyz),
	};

	if (distances.x < TESR_ShadowNearCenter.w) {
		if (distances.x < TESR_ShadowNearCenter.w * blend) return shadows.x;
		return lerp(shadows.x, shadows.y, smoothstep(TESR_ShadowNearCenter.w * blend, TESR_ShadowNearCenter.w, distances.x));
	}
	else if (distances.y < TESR_ShadowFarCenter.w) {
		if (distances.y < TESR_ShadowFarCenter.w * blend) return shadows.y;
		return lerp(shadows.y, 1.0f, smoothstep(TESR_ShadowFarCenter.w * blend, TESR_ShadowFarCenter.w, distances.y));
	}
	return 1.0f;
}

// 4x4 ordered dither matrix (ported from Oblivion Reloaded's VolumetricLight.fx.hlsl), indexed by
// screen pixel coordinates -- offsets each pixel's raymarch start position by a fraction of one
// step, so neighbouring pixels sample different points along their own ray. Preferred over an ad-hoc
// hash function: a known, standard ordered-dither pattern rather than a guessed one.
static const float4x4 DITHER_PATTERN = {
	0.0f,    0.5f,    0.125f,  0.625f,
	0.75f,   0.22f,   0.875f,  0.375f,
	0.1875f, 0.6875f, 0.0625f, 0.5625f,
	0.9375f, 0.4375f, 0.8125f, 0.3125f
};

// Glare's core math, extracted so both the standalone corona term and VolumetricCombine below can
// share it without duplicating it -- Volumetric no longer has its own separate glare technique the
// way GlareOnly used to be one; the corona is just one of the light contributions Combine adds in.
float3 ComputeGlare(float2 uv) {
	if (sunHeight >= 1 || GlareStrength <= 0) return 0;

	float sunset = pows(sunHeight, 8);
	float3 sunColor = linearize(TESR_SunColor).rgb + lerp(linearize(TESR_SunsetColor.rgb), 0, sunset);
	float glarePower = lerp(0.1, 8.0, sunset);

	float3 eyeDir = normalize(reconstructPosition(uv));
	float3 glare = pows(dot(TESR_ViewSpaceLightDir.xyz, eyeDir), 180) * glarePower;
	glare *= smoothstep(0, 0.01, sunHeight) * GlareStrength;

	return glare * sunColor * multiplier;
}

float4 VolumetricRaymarch(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord / scale;
	clip((uv <= 1) - 1);

	float depth = readDepth(uv);
	float3 eyeVector = toWorld(uv);
	float3 rayDir = normalize(eyeVector);
	eyeVector *= depth;
	float3 rayStart = TESR_CameraPosition.xyz;
	// Reuses the surface pixel's own world normal for every step's shadow-bias offset rather than
	// computing one per step -- there's no real normal in open air, and the bias only affects a
	// small anti-acne offset, harmless given the cascades are already filtered.
	float3 worldNormal = GetWorldNormal(uv);

	float rayLength = min(length(eyeVector), VolumetricMaxDistance);

	// Bound the march to the fog layer's height band [HeightCutoff - LayerThickness, HeightCutoff]
	// rather than the whole view ray -- most of a typical ray passes well above any ground-hugging
	// mist layer, so this concentrates samples where they're actually visible. Ray-plane intersection
	// against both band edges, sign-safe for looking up, down, or (near-)horizontally.
	float bandTop = VolumetricHeightCutoff;
	float bandBottom = VolumetricHeightCutoff - VolumetricLayerThickness;
	float dz = rayDir.z;
	float t0, t1;
	if (abs(dz) < 0.0001) {
		bool inBand = (rayStart.z >= bandBottom && rayStart.z <= bandTop);
		t0 = 0;
		t1 = inBand ? rayLength : 0;
	}
	else {
		float tBottom = (bandBottom - rayStart.z) / dz;
		float tTop = (bandTop - rayStart.z) / dz;
		t0 = clamp(min(tBottom, tTop), 0, rayLength);
		t1 = clamp(max(tBottom, tTop), 0, rayLength);
	}
	float bandLength = max(0, t1 - t0);
	float stepDist = bandLength / VolumetricSteps;

	float2 screenPos = uv / TESR_ReciprocalResolution.xy;
	float ditherOffset = DITHER_PATTERN[int(screenPos.x) % 4][int(screenPos.y) % 4];

	float accumLight = 0;
	float nearWeight = 1.0;
	// A real dynamic loop, not [unroll]: unrolling duplicated this loop's body -- two inlined
	// GetFogShadowValue calls, each with its own VSM/EVSM2/EVSM4 branch and float literals -- once
	// per step, and THAT (not the cascade count) is what was actually blowing the ps_3_0 224
	// constant-register budget (X4507). FlashlightBeam.fx.hlsl's own shadow/cookie raymarch already
	// uses [loop] for the same reason at a similar step count.
	[loop]
	for (int i = 0; i < VolumetricSteps; i++) {
		float t = t0 + (i + ditherOffset) * stepDist;
		float3 stepPos = rayStart + rayDir * t;
		float shadowVis = GetFogShadowVisibility(float4(stepPos, 1.0), worldNormal);

		if (shadowVis >= 1.0) {
			accumLight += nearWeight;
		}
		else {
			// Shadowed points still contribute a reduced, distance-limited amount rather than
			// nothing -- represents indirect/ambient scattering within the fog instead of a hard
			// binary lit/unlit switch.
			accumLight += nearWeight * saturate(1 - t / max(VolumetricShadowedCutoffDistance, 1));
		}
		nearWeight = max(0, nearWeight - VolumetricNearWeightFalloff / VolumetricSteps);
	}
	accumLight /= VolumetricSteps;
	accumLight *= VolumetricStrength;

	return float4(accumLight.xxx, 1.0f);
}

float4 VolumetricExpand(VSOUT IN) : COLOR0 {
	// Plain bilinear upsample, no depth-awareness -- matches Oblivion Reloaded's own Expand() pass,
	// which gets away without a depth-aware/edge-aware version. Worth revisiting only if halos show
	// up in practice.
	return tex2D(TESR_RenderedBuffer, IN.UVCoord * scale);
}

float4 VolumetricCombine(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float4 scene = linearize(tex2D(TESR_SourceBuffer, uv));
	float shaftLight = tex2D(TESR_RenderedBuffer, uv).r;

	// Directional gate toward the sun, same term Classic/Enhanced both use for their own ray
	// contribution -- without it, shaftLight (which is close to "fully lit" for most raymarched
	// points, since GetFogShadowVisibility falls back to 1.0 outside the shadow cascades' radius)
	// was being applied to every pixel regardless of view direction, not just ones looking toward
	// the sun, which is what washed the whole screen white instead of showing localized shafts.
	float3 eyeDir = normalize(reconstructPosition(uv));
	float heightAttenuation = TESR_GodRaysData.w ? lerp(0.2, 4.0, pows(sunHeight, 4)) : 1.0;
	float attenuation = pow(compress(shade(TESR_ViewSpaceLightDir.xyz, eyeDir)), 2.5) * heightAttenuation * (sunHeight < 1);

	float3 sunColor = GetSunColor(shade(TESR_SunDirection.xyz, blue.xyz), 1, TESR_SunAmount.x, TESR_SunColor.rgb, TESR_SunsetColor.rgb);
	float3 tintedShaft = shaftLight * sunColor * attenuation;
	float3 glareLight = ComputeGlare(uv); // already self-limited to a tight cone around the sun disk

	// Small output dither -- separate from VolumetricRaymarch's own per-pixel start-offset dither
	// above, which addresses step banding, not render-target quantization banding in the final value.
	float ditherNoise = (frac(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453) - 0.5) * (0.5 / 255.0);

	// Saturated here (unlike Oblivion Reloaded's own CombineLight, which leaves it unclamped) --
	// OR's raymarch stays naturally bounded near the shaft without a separate directional gate, ours
	// doesn't, so an unclamped light could still exceed 1 near the sun itself (HDR sunColor) and flip
	// `scene*(1-light)` negative there, which is what showed up as the brightest pixels "inverting".
	float3 light = saturate(tintedShaft + glareLight + ditherNoise);

	// Screen/over composite, not additive: light partially replaces the scene rather than only
	// adding to it, so it can't blow out highlights the way `color += light` can (matches Oblivion
	// Reloaded's own CombineLight).
	float3 color = scene.rgb * (1 - light) + light;
	color = delinearize(color);
	return float4(color, 1.0f);
}


technique Classic
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SkyMask();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 LightMask();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 RadialBlur(stepLength);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 RadialBlur(stepLength * stepLength);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 RadialBlur(stepLength * stepLength * stepLength);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		Pixelshader = compile ps_3_0 Combine();
	}
}

technique Enhanced
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 RayMaskEnhanced();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 LightShaftEnhanced();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurEnhanced();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombineEnhanced();
	}
}

technique Volumetric
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 VolumetricRaymarch();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 VolumetricExpand();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 VolumetricCombine();
	}
}