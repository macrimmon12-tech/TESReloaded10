// Volumetric Fog fullscreen shader for New Vegas Reloaded
//
// Single unified density/scattering term (replaces the old Simple+Height fog stack), anchored to
// vanilla weather fog (TESR_FogData/TESR_FogColor) via WeatherImpact, coupled to the sun shadow
// cascades and the SH sky ambient, animated with wind-driven procedural 3D noise. Distant fog
// (horizon Z-fighting/sky-seam matte) is kept as its own separate blend, since it solves a
// different problem (hiding a depth-buffer artifact, not modelling participating media).

float4 TESR_FogColor;
float4 TESR_FogData;  // weather values : x: fogNear, y:fogFar, z: sunglare, w:fogpower
float4 TESR_ReciprocalResolution;
float4 TESR_GameTime; // x: game time (ms), y: game hour, z: time, w: delta time
float4 TESR_SmoothedSunDir;
float4 TESR_SunPosition;
float4 TESR_SunColor;
float4 TESR_SunDiskColor;
float4 TESR_SunAmbient;
float4 TESR_HorizonColor;
float4 TESR_SkyLowColor;
float4 TESR_SkyColor; // top sky color
float4 TESR_SkyData; // x: athmosphere thickness, y: sun influence, z: sun strength w: sky strength
float4 TESR_SunsetColor; // color boost for sun when near the horizon
float4 TESR_SunAmount; // x: isDaytime
float4 TESR_PBRData; // z: direct light scale, w: ambient scale -- shared with objects for tonal consistency
float4 TESR_SkyIrradiance[9]; // order-2 SH sky irradiance, linear radiance (Sky.cpp, only fresh while Shaders.Sky is enabled)

// sun shadow cascade data -- registered globally by ShadowsExteriorEffect, same constants
// SunShadows.fx.hlsl consumes. Declared here independently since each .fx effect compiles on
// its own; see GetFogShadowVisibility below.
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformMiddle;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformLod;
float4 TESR_ShadowNearCenter;   // xyz: center (world space), w: radius
float4 TESR_ShadowMiddleCenter;
float4 TESR_ShadowFarCenter;
float4 TESR_ShadowLodCenter;
float4 TESR_ShadowFormatData; // x: mode (0 VSM, 1 EVSM2, 2 EVSM4), y: format bits per pixel
float4 TESR_ShadowBlur;       // x: 1 / atlas resolution
float4 TESR_ShadowFade;       // y: shadow maps active

// interior point lights -- same tracked-light arrays SnowAccumulation.fx.hlsl already consumes.
// First ShadowCubeMapsMax (12) slots of TESR_LightColor correspond to TESR_ShadowLightPosition
// (shadow-casting lights); the next 12 correspond to TESR_LightPosition (non-shadowed).
float4 TESR_ShadowLightPosition[12];
float4 TESR_LightPosition[12];
float4 TESR_LightColor[24];

// interior hero light (tier 2 shafts) -- picked in C++, see VolumetricFog.cpp. A and B are the
// current and previous hero, crossfaded by HeroBlend.x; w of either position is 0 when unset.
float4 TESR_VolumetricFogHeroLightA; // xyz: world position, w: radius
float4 TESR_VolumetricFogHeroLightB;
float4 TESR_VolumetricFogHeroColorA; // rgb: color, a: intensity
float4 TESR_VolumetricFogHeroColorB;
float4 TESR_VolumetricFogHeroBlend;  // x: crossfade blend (0=B, 1=A), y: InteriorGlowStrength, z: InteriorShaftStrength

float4 TESR_VolumetricFogDensity;    // x: BaseDensity, y: WeatherImpact, z: MorningFogDip, w: SunriseSunsetBoost
float4 TESR_VolumetricFogShape;      // x: HeightFalloff, y: MaxHeight, z: Extinction, w: Inscattering
float4 TESR_VolumetricFogWind;       // x: WindDirX, y: WindDirY, z: WindSpeed, w: NoiseScale
float4 TESR_VolumetricFogScatter;    // x: PhaseAsymmetry, y: ShadowStrength, z: NoiseStrength, w: HeightInfluence
float4 TESR_VolumetricFogWeather;    // x: WeatherFilterBlend (animated 0-1), y: isExterior, z: SkyAmbientAvailable, w: FogSaturation
float4 TESR_VolumetricFogAerial;     // x: AerialStrength, y: AerialRangeStart, z: AerialTintBlend, w: unused
float4 TESR_VolumetricFogAerialTint; // xyz: manual aerial tint override
float4 TESR_VolumetricFogDistant;    // x: DistantFogRange, y: DistantFogBlend, z: DistantFogHeight, w: EdgeAA
float4 TESR_VolumetricFogGlobal;     // x: Amount

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_RenderedBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowAtlas : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_FogHeroCubeMapA : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_FogHeroCubeMapB : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

/*Height-based fog settings*/
static const float FOG_GROUND = -10000;
static const float SUNINFLUENCE = 1 / TESR_SkyData.y;
static const float nearFog = TESR_FogData.x;
static const float farFog = TESR_FogData.y;
static const float FogPower = TESR_FogData.w;

// scale settings for easier tuning
static const float FogAmount = max(0, TESR_VolumetricFogGlobal.x);

static const float BaseDensity = max(0, TESR_VolumetricFogDensity.x);
static const float WeatherImpact = max(0, TESR_VolumetricFogDensity.y);
static const float MorningFogDip = saturate(TESR_VolumetricFogDensity.z);
static const float SunriseSunsetBoost = max(0, TESR_VolumetricFogDensity.w);

static const float HeightFalloff = max(0.0001, TESR_VolumetricFogShape.x);
static const float MaxHeight = TESR_VolumetricFogShape.y;
static const float Extinction = max(0, TESR_VolumetricFogShape.z) * 0.1;
static const float Inscattering = max(0, TESR_VolumetricFogShape.w) * 0.1;

static const float2 WindDirection = TESR_VolumetricFogWind.xy;
static const float WindSpeed = TESR_VolumetricFogWind.z;
static const float NoiseScale = max(0.0001, TESR_VolumetricFogWind.w);

static const float PhaseAsymmetry = saturate(TESR_VolumetricFogScatter.x);
static const float ShadowStrength = saturate(TESR_VolumetricFogScatter.y);
static const float NoiseStrength = saturate(TESR_VolumetricFogScatter.z);
// 1 = normal height-based density falloff (exterior default). 0 = no height dependence at all,
// density shaped purely by distance -- for interiors, where MaxHeight has no consistent meaning
// across arbitrary cell geometry unless an author explicitly tunes it for a known space.
static const float HeightInfluence = saturate(TESR_VolumetricFogScatter.w);

static const float WeatherFilterBlend = saturate(TESR_VolumetricFogWeather.x);
static const float isExterior = TESR_VolumetricFogWeather.y; // 0 or 1 to activate/cancel fog in interiors
static const float SkyAmbientAvailable = TESR_VolumetricFogWeather.z;
static const float FogSaturation = max(0, TESR_VolumetricFogWeather.w);

static const float AerialStrength = max(0, TESR_VolumetricFogAerial.x);
static const float AerialRangeStart = saturate(TESR_VolumetricFogAerial.y);
static const float AerialTintBlend = saturate(TESR_VolumetricFogAerial.z);

static const float DistantFogRange = exp(-4 * clamp(TESR_VolumetricFogDistant.x, 0.00000001, 1.0));
static const float DistantFogBlend = TESR_VolumetricFogDistant.y;
static const float DistantFogHeight = TESR_VolumetricFogDistant.z;
static const float EdgeAA = max(0, TESR_VolumetricFogDistant.w);

static const float HeroCrossfadeBlend = saturate(TESR_VolumetricFogHeroBlend.x);
static const float InteriorGlowStrength = max(0, TESR_VolumetricFogHeroBlend.y);
static const float InteriorShaftStrength = max(0, TESR_VolumetricFogHeroBlend.z);

// fixed tuning constant: how strongly a high density value pulls the fog's own color toward
// TESR_FogColor instead of the sky/ambient tint. Was two separate exposed settings
// (SimpleFogSkyColor/HeightFogSkyColor) before the two systems were unified.
static const float FogSkyColorCoeff = 6.0;

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Shadows.hlsl"
#include "Includes/Sky.hlsl"
#include "Includes/Normals.hlsl"

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


// hash based 3d value noise, same as SnowAccumulation.fx.hlsl's snow-coverage noise
// function taken from https://www.shadertoy.com/view/XslGRr - Inigo Quilez, 2013, CC BY-NC-SA 3.0
float hash(float n)
{
	return frac(sin(n) * 43758.5453);
}

float noise(float3 x)
{
	float3 p = floor(x);
	float3 f = frac(x);
	f = f * f * (3.0 - 2.0 * f);
	float n = p.x + p.y * 57.0 + 113.0 * p.z;

	return lerp(lerp(lerp(hash(n + 0.0), hash(n + 1.0), f.x),
			lerp(hash(n + 57.0), hash(n + 58.0), f.x), f.y),
			lerp(lerp(hash(n + 113.0), hash(n + 114.0), f.x),
			lerp(hash(n + 170.0), hash(n + 171.0), f.x), f.y), f.z);
}

// 3-octave FBM, recentered from noise()'s [0,1] range around a mid value so it can modulate
// density both up and down rather than only ever thinning it.
float fbm3(float3 p)
{
	float n = noise(p) * 0.55 + noise(p * 2.13) * 0.3 + noise(p * 4.7) * 0.15;
	return saturate(n * 1.4 - 0.2);
}


// order-2 SH sky irradiance reconstruction. Mirrors Shaders/Includes/SkyAmbient.hlsl's mode-0
// path, but returns LINEAR radiance (no final sqrt/gamma-2 encode) since this file already
// works in true linear space throughout via linearize()/delinearize().
float3 SkyIrradianceLinear(float3 worldNormal)
{
	float3 n = worldNormal;
	float3 irradiance = TESR_SkyIrradiance[0].rgb
		 + TESR_SkyIrradiance[1].rgb * n.y
		 + TESR_SkyIrradiance[2].rgb * n.z
		 + TESR_SkyIrradiance[3].rgb * n.x
		 + TESR_SkyIrradiance[4].rgb * (n.x * n.y)
		 + TESR_SkyIrradiance[5].rgb * (n.y * n.z)
		 + TESR_SkyIrradiance[6].rgb * (3.0f * n.z * n.z - 1.0f)
		 + TESR_SkyIrradiance[7].rgb * (n.x * n.z)
		 + TESR_SkyIrradiance[8].rgb * (n.x * n.x - n.y * n.y);
	return max(irradiance, 0.0f);
}


#define stepnum 32
// exponential fog based on https://iquilezles.org/articles/fog/
//  (a/b) * exp(-ro.y*b) * (1.0-exp(-t*rd.y*b))/rd.y;
float getHeightFog(float distance, float falloff, float3 worldPos, float heightOffset){
	float3 eyeVector = worldPos - TESR_CameraPosition.xyz;
	float3 step = eyeVector / stepnum;
	float stepDist = length(step);

	float3 pos = TESR_CameraPosition.xyz - float3(0, 0, FOG_GROUND + heightOffset * 1000);
	float fog = 0;
	[unroll]
	for (int i = 0; i < stepnum; i++){
		pos += step;
		fog += exp(-falloff * pos.z) * stepDist;
	}

	return fog * (length(eyeVector) / distance); // apply distance modifiers from weather/settings
}

float3 mixHeightFog(float3 color, float3 fogColor, float3 extinctionColor, float3 inscatteringColor, float distance, float density, float falloff, float3 worldPos, float offset){
	float fog = density * 0.00001 * getHeightFog(distance, falloff * 0.0001, worldPos, offset);
	float3 extColor = fog * extinctionColor;
	float3 insColor = fog * inscatteringColor;
	return color * saturate(1 - extColor) + fogColor * saturate(insColor);
}

// Flat, distance-only density -- no height term at all. This is the HeightInfluence=0 endpoint;
// it is NOT the same as feeding falloff=0 into getHeightFog/mixHeightFog above, since that
// function's outer (length(eyeVector)/distance) normalization is built assuming the integral is
// doing real work and doesn't collapse to a clean distance-only result at falloff=0.
float3 getFogFlat(float distance, float3 density){
	return 1 - exp(-distance * density * 0.0001);
}

float3 mixFogFlat(float3 color, float3 fogColor, float3 extinctionColor, float3 inscatteringColor, float distance, float density){
	float3 extColor = getFogFlat(distance, density * extinctionColor);
	float3 insColor = getFogFlat(distance, density * inscatteringColor);
	return color * saturate(1 - extColor) + fogColor * saturate(insColor);
}


float getSky(float2 uv) {
	float4 color = linearize(tex2D(TESR_SourceBuffer, uv));

	float distance = 0.5 * TESR_ReciprocalResolution.xy;
	distance *= 1;

	float3 coeffs = float3(float2(1, -1) * distance, 0.9961);
	float depth = readDepth(uv) / farZ >= coeffs.z;
	float depth1 = readDepth(uv + coeffs.xx) / farZ >= coeffs.z;
	float depth2 = readDepth(uv + coeffs.xy) / farZ >= coeffs.z;
	float depth3 = readDepth(uv + coeffs.yx) / farZ >= coeffs.z;
	float depth4 = readDepth(uv + coeffs.yy) / farZ >= coeffs.z;

	float total = (depth || depth1 || depth2 || depth3 || depth4) * luma(color); // we scale the result with the point luma since sky tends to be brighter

	return saturate(total);
}

// scale the fog color between sky, purefog, and sun contribution
float4 fogColor(float4 skyColor, float4 pureFogColor, float fogStrength, float skyColorCoeff, float4 sunScattering, float saturation){
	float4 fogColor = lerp(skyColor, pureFogColor, saturate(pows(fogStrength, skyColorCoeff)));    // modulate between sky and pure fog color if strength is high
	fogColor += sunScattering;                              									   // add sun influence
	fogColor = lerp(luma(fogColor), fogColor, saturation/(1 + fogStrength));                       // boost fog color saturation
	return fogColor;
}


// Adapted from SunShadows.fx.hlsl's GetLightAmount, trimmed to a single atlas tap per cascade
// (the atlas is already Gaussian-prefiltered, so single-tap VSM/EVSM is the established default
// there too) and sampled once per pixel rather than per raymarch step -- GetLightAmount's own
// unconditional 4-cascade evaluation makes it too costly to call many times per pixel. Separate
// effects compile independently, so this can't be shared with SunShadows.fx.hlsl directly.
float4 ScreenCoordToTexCoord(float4 coord){
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

	float4 radii = { TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w };
	float4 texelWorld = 4.0f * radii * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
	float4 offsetDistance = offsetScale * 2.5f * texelWorld; // 2.5 = normal-offset bias in texels, matches SunShadows.fx.hlsl

	float bias = (TESR_ShadowFormatData.x == 0.0f ? 0.00001f : 0.01f) * (1.0f + offsetScale);
	const float blend = 0.9f;

	float4 shadows = {
		GetFogShadowValue(TESR_ShadowCameraToLightTransformNear,   float4(positionWS.xyz + offsetDistance.x * normal, 1.0f), 0.0, 0.0, bias),
		GetFogShadowValue(TESR_ShadowCameraToLightTransformMiddle, float4(positionWS.xyz + offsetDistance.y * normal, 1.0f), 0.5, 0.0, bias),
		GetFogShadowValue(TESR_ShadowCameraToLightTransformFar,    float4(positionWS.xyz + offsetDistance.z * normal, 1.0f), 0.0, 0.5, bias),
		GetFogShadowValue(TESR_ShadowCameraToLightTransformLod,    float4(positionWS.xyz + offsetDistance.w * normal, 1.0f), 0.5, 0.5, bias),
	};

	float4 distances = {
		length(positionWS.xyz - TESR_ShadowNearCenter.xyz),
		length(positionWS.xyz - TESR_ShadowMiddleCenter.xyz),
		length(positionWS.xyz - TESR_ShadowFarCenter.xyz),
		length(positionWS.xyz - TESR_ShadowLodCenter.xyz),
	};

	if (distances.x < TESR_ShadowNearCenter.w) {
		if (distances.x < TESR_ShadowNearCenter.w * blend) return shadows.x;
		return lerp(shadows.x, shadows.y, smoothstep(TESR_ShadowNearCenter.w * blend, TESR_ShadowNearCenter.w, distances.x));
	}
	else if (distances.y < TESR_ShadowMiddleCenter.w) {
		if (distances.y < TESR_ShadowMiddleCenter.w * blend) return shadows.y;
		return lerp(shadows.y, shadows.z, smoothstep(TESR_ShadowMiddleCenter.w * blend, TESR_ShadowMiddleCenter.w, distances.y));
	}
	else if (distances.z < TESR_ShadowFarCenter.w) {
		if (distances.z < TESR_ShadowFarCenter.w * blend) return shadows.z;
		return lerp(shadows.z, shadows.w, smoothstep(TESR_ShadowFarCenter.w * blend, TESR_ShadowFarCenter.w, distances.z));
	}
	else if (distances.w < TESR_ShadowLodCenter.w) {
		if (distances.w < TESR_ShadowLodCenter.w * blend) return shadows.w;
		return lerp(shadows.w, 1.0f, smoothstep(TESR_ShadowLodCenter.w * blend, TESR_ShadowLodCenter.w, distances.w));
	}
	return 1.0f;
}


float4 VolumetricFog(VSOUT IN) : COLOR0
{
	float4 color = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	float4 pureFogColor = linearize(TESR_FogColor);

	float depth = readDepth(IN.UVCoord);
	float isDayTime = smoothstep(0.4, 0.8, TESR_SunAmount.x);
	float isDayTimeFog = smoothstep(0.1, 0.6, TESR_SunAmount.x);

	float3 eyeVector = toWorld(IN.UVCoord);
	float3 eyeDirection = normalize(eyeVector);
	eyeVector *= depth;
	float3 worldPos = eyeVector + TESR_CameraPosition.xyz;

	float normalizedDepth = length(eyeVector) / farZ; // make sure depth is the same at the center and edges of the screen
	float fogPower = lerp(1, FogPower, saturate(WeatherImpact));
	float fogDepth = pows(normalizedDepth, fogPower) * farZ;

	float isSky = getSky(IN.UVCoord);
	// getSky's 5-tap test also lights up on pixels bordering the sky depth threshold, so this
	// doubles as an edge-dilated mask usable for anti-aliasing that boundary further down.
	float skyMaskFactor = lerp(1.0, 1.0 - isSky, WeatherFilterBlend); // weather can disable the sky exclusion so fog blends fully into an overcast sky

	// Pure depth-threshold sky test, unweighted by luma -- getSky()'s own luma weighting makes
	// isSky fall toward 0 on a dark sky (night, heavy overcast) even though the pixel genuinely
	// is the skydome, which let the aerial tint paint over it. The aerial exclusion needs a hard
	// gate regardless of sky brightness. Reuses `depth`, already sampled above via readDepth().
	float isSkyDome = depth / farZ >= 0.9961;

	// ---- vanilla-anchored density, derived from the active weather's own authored fog shape ----
	float vanillaStrength = pows((saturate(1 - farFog / farZ) + saturate(1 - nearFog / farZ)) / 2, 2) / (FogPower + 1);

	// ---- NVR-driven density: time-of-day curve, sunrise/sunset boost, animated 3D noise ----
	float sunsetBump = sin(saturate(TESR_SunAmount.x) * PI) * isExterior;
	float noonTrough = 1 - saturate(abs(TESR_GameTime.y - 12) / 6); // 0 at 6am/6pm, 1 at solar noon
	float timeOfDayScale = lerp(1.0, lerp(1.0, 1.0 - MorningFogDip, noonTrough), isExterior);

	float3 windOffset = float3(WindDirection * WindSpeed * TESR_GameTime.x * 0.002, 0);
	float noiseVal = fbm3((worldPos + windOffset) / (1500 * NoiseScale));

	float nvrDensity = BaseDensity * timeOfDayScale * lerp(1.0, noiseVal, NoiseStrength);
	nvrDensity = nvrDensity * WeatherFilterBlend + SunriseSunsetBoost * sunsetBump * WeatherFilterBlend;

	float strength = max(0, nvrDensity + WeatherImpact * vanillaStrength);

	// ---- shadow-coupled sun in-scattering, sampled once per pixel at the fog's terminating point ----
	float3 worldNormal = GetWorldNormal(IN.UVCoord);
	float shadowVisibility = 1.0;
	[branch]
	if (isExterior > 0.5 && ShadowStrength > 0.0 && TESR_ShadowFade.y > 0.5) {
		shadowVisibility = lerp(1.0, GetFogShadowVisibility(float4(worldPos, 1.0), worldNormal), ShadowStrength);
	}

	// default values used by interiors
	float distantFog = 0.0, distantHeightFade = 0.0;
	float4 skyColor = pureFogColor;
	float4 sun = black;

	// sun/sky/distant fog coloring specific to exteriors
	if (isExterior){
		float3 up = blue.xyz;
		float sunHeight = shade(TESR_SunPosition.xyz, up);
		float sunDir = dot(eyeDirection, TESR_SunPosition.xyz);
		float sunInfluence = pows(compress(sunDir), SUNINFLUENCE);
		float4 sunColorV = float4(GetSunColor(sunHeight, 1, TESR_SunAmount.x, TESR_SunDiskColor.rgb, TESR_SunsetColor.rgb), 1);
		sunColorV *= isDayTime * isExterior;

		skyColor.rgb = GetSkyColor(0.5, 1, sunHeight, sunInfluence, TESR_SkyData.z, TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, black.rgb) * TESR_SunsetColor.w;

		// add sun influence/scattering, narrowed/widened by PhaseAsymmetry, gated by the shadow cascades
		float sunStrength = TESR_FogData.z / (1 + 2 * strength);
		sunStrength = lerp(sunStrength, max(sunStrength * 2, 10), normalizedDepth);

		float sunScattering = pows(compress(sunDir), lerp(8.0, 1.0, PhaseAsymmetry) + sunStrength);
		sunScattering *= pow(1 - sunHeight, 2) * isDayTimeFog * TESR_FogData.z;
		sun = sunColorV * sunScattering * shadowVisibility * TESR_PBRData.z * 100;

		distantFog = pows(smoothstep(DistantFogRange, 1.0, normalizedDepth), 0.5);
		distantHeightFade = (DistantFogHeight == 0) ? (1.0 - isSky) : exp(-worldPos.z / (80000 * DistantFogHeight));
	}
	else {
		// interior point-light in-scattering, the indoor analog of the sun term above: tier 1 is
		// a cheap unshadowed glow from every tracked light (matches SnowAccumulation.fx.hlsl's
		// own point-light loop), tier 2 is real shadowed beams from the single "hero" light
		// picked in C++ (VolumetricFog.cpp), crossfaded between its current and previous winner.
		float3 glow = 0;
		for (int i = 0; i < 12; i++) {
			glow += GetPointLightContribution(float4(worldPos, 1), TESR_ShadowLightPosition[i], float4(worldNormal, 1)) * linearize(float4(TESR_LightColor[i].rgb * TESR_LightColor[i].a, 1)).rgb;
			glow += GetPointLightContribution(float4(worldPos, 1), TESR_LightPosition[i], float4(worldNormal, 1)) * linearize(float4(TESR_LightColor[12 + i].rgb * TESR_LightColor[12 + i].a, 1)).rgb;
		}
		glow *= InteriorGlowStrength;

		float3 heroColorA = linearize(float4(TESR_VolumetricFogHeroColorA.rgb * TESR_VolumetricFogHeroColorA.a, 1)).rgb;
		float3 heroColorB = linearize(float4(TESR_VolumetricFogHeroColorB.rgb * TESR_VolumetricFogHeroColorB.a, 1)).rgb;
		float heroAmountA = GetPointLightAmount(TESR_FogHeroCubeMapA, float4(worldPos, 1), TESR_VolumetricFogHeroLightA, float4(worldNormal, 1));
		float heroAmountB = GetPointLightAmount(TESR_FogHeroCubeMapB, float4(worldPos, 1), TESR_VolumetricFogHeroLightB, float4(worldNormal, 1));
		float3 shafts = lerp(heroAmountB * heroColorB, heroAmountA * heroColorA, HeroCrossfadeBlend) * InteriorShaftStrength;

		sun = float4(glow + shafts, 1);
	}

	// ---- sky ambient in-scattering (SH-reconstructed, or flat fallback), unshadowed ----
	float3 ambientColor = SkyAmbientAvailable > 0.5
		? SkyIrradianceLinear(float3(0, 0, 1))
		: linearize(TESR_SkyColor).rgb;
	ambientColor *= TESR_PBRData.w;

	float4 ambientSkyColor = float4(lerp(ambientColor, skyColor.rgb, saturate(WeatherImpact)), 1);

	float4 fogColorFinal = fogColor(ambientSkyColor, pureFogColor, strength, FogSkyColorCoeff, sun, FogSaturation);
	// Blend between flat (distance-only, no height dependence) and height-integrated density.
	// HeightInfluence=1 (exterior default) reduces to exactly the prior height-only behavior;
	// HeightInfluence=0 (interior default) gives uniform density regardless of MaxHeight, safe
	// for a generic preset with no per-cell tuning. Per-cell presets can raise HeightInfluence
	// alongside a hand-tuned MaxHeight for spaces where a real gradient is known to make sense.
	float3 flatFogged = mixFogFlat(color.rgb, fogColorFinal.rgb, Extinction, Inscattering, fogDepth, strength);
	float falloffArg = 1.5 / (fogPower * HeightFalloff);
	float3 heightFogged = mixHeightFog(color.rgb, fogColorFinal.rgb, Extinction, Inscattering, fogDepth, strength, falloffArg, worldPos, MaxHeight);
	float3 fogged = lerp(flatFogged, heightFogged, HeightInfluence);
	float4 finalColor = float4(lerp(color.rgb, fogged, skyMaskFactor), 1);

	// ---- aerial perspective: mid-to-far distance tint on non-sky terrain ----
	// Gated by isDayTimeFog: aerial haze is a daylight-scattering phenomenon, and the manual
	// AerialTint override in particular is a fixed color that doesn't dim on its own at night --
	// without this gate it reads as a glow against an otherwise-dark night scene.
	float aerialFactor = smoothstep(AerialRangeStart, 1.0, normalizedDepth) * (1.0 - isSkyDome) * isExterior;
	float3 aerialTint = lerp(ambientColor, linearize(TESR_VolumetricFogAerialTint).rgb, AerialTintBlend);
	finalColor.rgb = lerp(finalColor.rgb, aerialTint, aerialFactor * AerialStrength * skyMaskFactor * isDayTimeFog);

	// ---- distant fog: horizon Z-fighting/sky-seam matte ----
	finalColor = lerp(finalColor, skyColor, distantFog * saturate(DistantFogBlend) * distantHeightFade * isExterior);

	// inverted edge term: getSky's dilated mask specifically feathers the terrain/sky silhouette,
	// independent of the weather-driven sky mask above since this is an anti-aliasing fix, not a
	// density effect -- it should keep working in rain/overcast just as much as on clear days.
	finalColor.rgb = lerp(finalColor.rgb, skyColor.rgb, isSky * EdgeAA * isExterior);

	finalColor = max(lerp(color, finalColor, FogAmount), 0.0f);

	return delinearize(finalColor);
}


technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 VolumetricFog();
	}
}
