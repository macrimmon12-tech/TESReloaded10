// Volumetric Light shafts for New Vegas Reloaded.
//
// Ray-marched sun light shaft, ported from arafuse/tes-reloaded's OblivionReloaded/Shaders/
// VolumetricLight/VolumetricLight.fx.hlsl (credited there to alexandre-pestana.com/volumetric-lights,
// andrew-pham.blog volumetric lighting, shader-tutorial.dev dithering, and a flow-noise function
// from https://www.shadertoy.com/view/MtcGRl). The ray march, Henyey-Greenstein scattering,
// flow-noise animated fog, and dithered start offset are unchanged; the shadow lookup is
// rewritten against this fork's VSM/EVSM cascade atlas (near/middle/far/lod, cross-faded) instead
// of the source's plain near/far depth-compare maps.

float4 TESR_ReciprocalResolution;
float4 TESR_GameTime; // z: running time tick, used to scroll the animated fog noise
float4 TESR_SmoothedSunDir;
float4 TESR_SunColor;
float4 TESR_ShadowFade; // x: sunrise/sunset fade, y: shadow maps active
float4 TESR_ShadowData; // y: darkness

float4 TESR_VolumetricLightData1; // xyz: scatter color tint, w: accum distance cutoff
float4 TESR_VolumetricLightData2; // xyz: wind direction, w: fog power
float4 TESR_VolumetricLightData3; // x: strength (0 = off, also the Combine gate), y: fog density, z: height, w: anisotropy
float4 TESR_VolumetricLightData4; // x: sky scatter strength, y: dither toggle, z: unused, w: unused

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowAtlas : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Shadows.hlsl"

// --- Cascade shadow lookup -----------------------------------------------------------------
// Mirrors SunShadows.fx.hlsl's GetLightAmount (same registered constants, same atlas), copied
// rather than shared: the two effects compile independently and SunShadows.fx.hlsl already
// documents this "mirrored, not shared" pattern for the game-shader/effect split. Keep the two
// in step if cascade selection, bias, or atlas layout ever change.
row_major float4x4 TESR_ShadowCameraToLightTransformNear;
row_major float4x4 TESR_ShadowCameraToLightTransformMiddle;
row_major float4x4 TESR_ShadowCameraToLightTransformFar;
row_major float4x4 TESR_ShadowCameraToLightTransformLod;
float4 TESR_ShadowNearCenter;
float4 TESR_ShadowMiddleCenter;
float4 TESR_ShadowFarCenter;
float4 TESR_ShadowLodCenter;
float4 TESR_ShadowFormatData; // x: mode, y: format bits
float4 TESR_ShadowBlur; // x: 1 / atlas resolution

static const float ShadowMode = TESR_ShadowFormatData.x;
static const float ShadowFormatBits = TESR_ShadowFormatData.y;
static const float ShadowNormalBiasTexels = 2.5f;
static const float ShadowSlopeBias = 1.0f;

float4 ScreenCoordToTexCoord(float4 coord) {
    coord.xyz /= coord.w;
    coord.x = coord.x * 0.5f + 0.5f;
    coord.y = coord.y * -0.5f + 0.5f;
    return coord;
}

float SampleShadowMoments(float2 uv, out float4 moments) {
    moments = tex2Dlod(TESR_ShadowAtlas, float4(uv, 0.0f, 0.0f));
    return 1.0f;
}

float GetShadowValue(float4x4 lightTransform, float4 coord, float offsetX, float offsetY, float bias, float bleedReduction) {
    float4 lightSpaceCoord = ScreenCoordToTexCoord(mul(coord, lightTransform));
    lightSpaceCoord.xy *= 0.5f;
    lightSpaceCoord.x += offsetX;
    lightSpaceCoord.y += offsetY;

    float4 moments;
    SampleShadowMoments(lightSpaceCoord.xy, moments);

    [branch]
    if (ShadowMode == 0.0f)
        return GetLightAmountValueVSM(moments.xy, lightSpaceCoord.z, bias, bleedReduction);
    else if (ShadowMode == 1.0f)
        return GetLightAmountValueEVSM2(moments.xy, lightSpaceCoord.z, bias, bleedReduction, ShadowFormatBits);
    else
        return GetLightAmountValueEVSM4(moments, lightSpaceCoord.z, bias, bleedReduction, ShadowFormatBits);
}

// 1.0 in full light, towards 0 in shadow. positionWS is camera-relative world position
// (TESR_CameraPosition already folded in by the caller, matching GetShadowWorldPos elsewhere).
float GetSunShadowAmount(float3 positionWS, float3 normal) {
    if (!TESR_ShadowFade.y) return 1.0f;

    float NdotL = dot(normal, TESR_SmoothedSunDir.xyz);
    float offsetScale = saturate(1.0f - NdotL);

    float4 radii = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w);
    float4 texelWorld = 4.0f * radii * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
    float4 offsetDistance = offsetScale * ShadowNormalBiasTexels * texelWorld;

    float bias = (ShadowMode == 0.0f ? 0.00001f : 0.01f) * (1.0f + ShadowSlopeBias * offsetScale);
    const float blend = 0.9f;

    float4 distances = float4(
        length(positionWS - TESR_ShadowNearCenter.xyz),
        length(positionWS - TESR_ShadowMiddleCenter.xyz),
        length(positionWS - TESR_ShadowFarCenter.xyz),
        length(positionWS - TESR_ShadowLodCenter.xyz));

#define VL_SHADOW_TAP_NEAR   GetShadowValue(TESR_ShadowCameraToLightTransformNear,   float4(positionWS + offsetDistance.x * normal, 1.0f), 0.0f, 0.0f, bias, 0.1f)
#define VL_SHADOW_TAP_MIDDLE GetShadowValue(TESR_ShadowCameraToLightTransformMiddle, float4(positionWS + offsetDistance.y * normal, 1.0f), 0.5f, 0.0f, bias, 0.2f)
#define VL_SHADOW_TAP_FAR    GetShadowValue(TESR_ShadowCameraToLightTransformFar,    float4(positionWS + offsetDistance.z * normal, 1.0f), 0.0f, 0.5f, bias, 0.6f)
#define VL_SHADOW_TAP_LOD    GetShadowValue(TESR_ShadowCameraToLightTransformLod,    float4(positionWS + offsetDistance.w * normal, 1.0f), 0.5f, 0.5f, bias, 0.8f)

    float shadow = 1.0f;
    [branch] if (distances.x < radii.x) {
        [branch] if (distances.x < radii.x * blend)
            shadow = VL_SHADOW_TAP_NEAR;
        else
            shadow = lerp(VL_SHADOW_TAP_NEAR, VL_SHADOW_TAP_MIDDLE, smoothstep(radii.x * blend, radii.x, distances.x));
    }
    else if (distances.y < radii.y) {
        [branch] if (distances.y < radii.y * blend)
            shadow = VL_SHADOW_TAP_MIDDLE;
        else
            shadow = lerp(VL_SHADOW_TAP_MIDDLE, VL_SHADOW_TAP_FAR, smoothstep(radii.y * blend, radii.y, distances.y));
    }
    else if (distances.z < radii.z) {
        [branch] if (distances.z < radii.z * blend)
            shadow = VL_SHADOW_TAP_FAR;
        else
            shadow = lerp(VL_SHADOW_TAP_FAR, VL_SHADOW_TAP_LOD, smoothstep(radii.z * blend, radii.z, distances.z));
    }
    else if (distances.w < radii.w) {
        shadow = lerp(VL_SHADOW_TAP_LOD, 1.0f, smoothstep(radii.w * blend, radii.w, distances.w));
    }

#undef VL_SHADOW_TAP_NEAR
#undef VL_SHADOW_TAP_MIDDLE
#undef VL_SHADOW_TAP_FAR
#undef VL_SHADOW_TAP_LOD

    shadow = saturate(shadow);
    shadow = lerp(shadow, 1.0f, saturate(TESR_ShadowFade.x));
    return shadow;
}

// --- Ray march setup -------------------------------------------------------------------------

static const float4x4 DITHER_PATTERN = { 0.0f, 0.5f, 0.125f, 0.625f, 0.75f, 0.22f, 0.875f, 0.375f, 0.1875f, 0.6875f, 0.0625f, 0.5625f, 0.9375f, 0.4375f, 0.8125f, 0.3125f };

static const int MARCH_NUM = 14;
static const float SCATTERING = 0.1f;
static const float SCATTERING_SKY = 0.6f;
static const float NOISE_GRANULARITY = 0.5 / 255.0;
static const float RAY_LENGTH_MAX = 20000.0f;

static const float strength = TESR_VolumetricLightData3.x;
static const float fogDensity = TESR_VolumetricLightData3.y;
static const float HEIGHT = TESR_VolumetricLightData3.z;
static const float anisotropy = TESR_VolumetricLightData3.w;
static const float skyScatterStrength = TESR_VolumetricLightData4.x;
static const bool ditherEnabled = TESR_VolumetricLightData4.y > 0.5f;
static const float accumLightStrength = 3.0f;

struct VSOUT {
    float4 vertPos : POSITION;
    float2 UVCoord : TEXCOORD0;
};

struct VSIN {
    float4 vertPos : POSITION0;
    float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN) {
    VSOUT OUT = (VSOUT) 0.0f;
    OUT.vertPos = IN.vertPos;
    OUT.UVCoord = IN.UVCoord;
    return OUT;
}

float2 GetGradient(float2 pos, float t) {
    float r = rand(pos);
    float angle = 6.283185 * r + 4.0 * t * r;
    return float2(cos(angle), sin(angle));
}

float noise(float3 pos) {
    float2 i = floor(pos.xy);
    float2 f = pos.xy - i;
    float2 blend = f * f * (3.0 - 2.0 * f);
    float noiseVal =
        lerp(
            lerp(
                dot(GetGradient(i + float2(0, 0), pos.z), f - float2(0, 0)),
                dot(GetGradient(i + float2(1, 0), pos.z), f - float2(1, 0)),
                blend.x),
            lerp(
                dot(GetGradient(i + float2(0, 1), pos.z), f - float2(0, 1)),
                dot(GetGradient(i + float2(1, 1), pos.z), f - float2(1, 1)),
                blend.x),
        blend.y
    );
    return noiseVal / 0.7;
}

float flowNoise(float3 uvw) {
    return noise(uvw * 4.0) / 3.0f;
}

// pow(g, 1.5); fxc does not fold this on its own.
float Pow1_5(float g) {
    return g * sqrt(g);
}

float ComputeScatteringSky(float lightDotView) {
    float result = 1.0f - SCATTERING_SKY * SCATTERING_SKY;
    float g = 1.0f + SCATTERING_SKY * SCATTERING_SKY - (2.0f * SCATTERING_SKY) * lightDotView;
    result /= (4.0f * PI * Pow1_5(g));
    return result;
}

// Sky and ground scattering differ only in how far the media term may raise the
// Henyey-Greenstein parameter; the march picks the ceiling once outside the loop.
float ComputeScatteringClamped(float lightDotView, float media, float ceiling) {
    float scatter = min(SCATTERING + media, ceiling);
    float result = 1.0f - scatter * scatter;
    float g = 1.0f + scatter * scatter - (2.0f * scatter) * lightDotView;
    result /= (4.0f * PI * Pow1_5(g));
    return result;
}

float ComputeScattering(float lightDotView, float media) {
    return ComputeScatteringClamped(lightDotView, media, 0.5f);
}

// Low-resolution ray march: walks the view ray from the camera to the surface (or a capped
// height plane for sky pixels), sampling the sun shadow atlas at each step so light shafts are
// actually occluded by geometry between the camera and the sun -- not just a flat fog term.
float4 VolumetricLight(VSOUT IN) : COLOR0 {
    float2 uv = IN.UVCoord.xy;

    float depth = readDepth(uv);
    float3 cameraVector = toWorld(uv) * depth;
    float3 shadowWorldPosition = TESR_CameraPosition.xyz + cameraVector;

    bool inFog = TESR_CameraPosition.z < HEIGHT;
    float stepHeight = 2500.0f;

    float3 startPosition = TESR_CameraPosition.xyz;
    float3 noiseStartPosition = startPosition;
    float3 endPosition = shadowWorldPosition;
    float3 rayVector = endPosition - startPosition;

    float rayLength = length(rayVector);
    float noiseRayLength = rayLength;
    float3 rayDirection = rayVector / max(rayLength, 0.001f);
    rayLength = min(rayLength, lerp(RAY_LENGTH_MAX, rayLength, smoothstep(HEIGHT - (stepHeight - 600), HEIGHT, TESR_CameraPosition.z)));
    noiseRayLength = min(noiseRayLength, RAY_LENGTH_MAX);

    float nearModifier = 0.0f;
    bool isSky = depth > (farZ * 0.99f);
    if (isSky) nearModifier = 3.5f;

    float noiseStepLength = noiseRayLength / MARCH_NUM;
    float stepLength = rayLength / MARCH_NUM;
    float3 noiseStep = rayDirection * noiseStepLength;
    float3 step = rayDirection * stepLength;

    float3 currentPosition = startPosition;
    float3 noiseCurrentPosition = startPosition;

    if (!inFog) {
        currentPosition = startPosition + (step * MARCH_NUM);
        noiseCurrentPosition = startPosition + (noiseStep * MARCH_NUM);
        startPosition = currentPosition;
        noiseStartPosition = noiseCurrentPosition;
        step *= -1;
        noiseStep *= -1;
    }

    float ditherOffset = ditherEnabled
        ? DITHER_PATTERN[int(abs(uv.x) * (1.0f / TESR_ReciprocalResolution.x)) % 4][int(abs(uv.y) * (1.0f / TESR_ReciprocalResolution.y)) % 4]
        : 0.5f;
    currentPosition += step * ditherOffset;
    noiseCurrentPosition += noiseStep * ditherOffset;

    float3 accumLight = 0.0f.xxx;
    float3 windOffset = TESR_VolumetricLightData2.xyz * (TESR_GameTime.z / 10000.0f);

    float lightDotView = dot(rayDirection, TESR_SmoothedSunDir.xyz);
    float3 lightColor = TESR_VolumetricLightData1.xyz * TESR_SunColor.rgb;
    float scatterCeiling = isSky ? 1.0f : 0.5f;
    float3 shadowedScatter = ComputeScatteringClamped(lightDotView, 0.0f, 0.5f).xxx * lightColor;

    [loop]
    for (int i = 0; i < MARCH_NUM; i++) {
        float Shadow = GetSunShadowAmount(currentPosition, TESR_SmoothedSunDir.xyz);

        float3 noisePosition = (noiseCurrentPosition / 1500.0f) - windOffset;
        float fog = lerp(saturate(flowNoise(noisePosition)), 0.1f, saturate((distance(currentPosition, TESR_CameraPosition.xyz) / 50000.0f)));
        fog *= fogDensity;

        float scatterFog = fog;
        fog = fog * (TESR_VolumetricLightData1.xyz * 2);
        fog = fog * max(nearModifier, 1);
        fog = (fog / 6.0f);

        float heightTransition = lerp(1, 0, smoothstep(HEIGHT - stepHeight, HEIGHT, currentPosition.z));

        float3 litScatter = (ComputeScatteringClamped(lightDotView, scatterFog.x, scatterCeiling).xxx * lightColor) + fog;
        float3 shadowedContribution = (shadowedScatter * (1 - saturate(TESR_VolumetricLightData1.w / max(distance(currentPosition, TESR_CameraPosition.xyz), 0.001f)))) + fog;
        accumLight += lerp(shadowedContribution, litScatter, Shadow) * heightTransition;

        currentPosition += step;
        noiseCurrentPosition += noiseStep;

        if (currentPosition.z > HEIGHT && startPosition.z < HEIGHT) {
            float3 vec = inFog ? currentPosition - startPosition : startPosition - currentPosition;
            float rLength = length(vec);
            float nrLength = min(rLength, RAY_LENGTH_MAX);
            float3 rDir = vec / max(rLength, 0.001f);
            float newStepLength = rLength / max(MARCH_NUM - i, 1);
            float noiseNewStepLength = nrLength / max(MARCH_NUM - i, 1);
            step = rDir * newStepLength;
            noiseStep = rDir * noiseNewStepLength;
            if (!inFog) {
                step *= -1;
                noiseStep *= -1;
            }
            currentPosition = startPosition + step;
            noiseCurrentPosition = noiseStartPosition + noiseStep;
        }
        nearModifier -= 0.2f;
    }

    accumLight /= isSky ? MARCH_NUM : lerp(MARCH_NUM * 1.10, MARCH_NUM * 0.85f, saturate(rayLength / RAY_LENGTH_MAX));

    float fogCoeff = 1.0f - saturate(length(cameraVector) / max(TESR_VolumetricLightData1.w, 1.0f));
    accumLight *= accumLightStrength * fogCoeff * strength;
    accumLight += lerp(-NOISE_GRANULARITY, NOISE_GRANULARITY, rand(uv));

    return float4(max(accumLight, 0.0f), 1.0f);
}

// Full resolution: upsamples the low-res march (bilinear, via the sampler state above) and adds
// a cheap non-marched sky-ambient glow so the horizon doesn't look flat where the march's own
// per-pixel cost would be wasted on sky that never occludes anything.
float4 VolumetricLightSky(VSOUT IN) : COLOR0 {
    float2 uv = IN.UVCoord.xy;
    float3 color = tex2D(TESR_RenderedBuffer, uv).rgb;

    float depth = readDepth(uv);
    bool isSky = depth > (farZ * 0.99f);

    if (isSky) {
        float3 rayDirection = normalize(toWorld(uv));
        float scatter = ComputeScatteringSky(dot(rayDirection, TESR_SmoothedSunDir.xyz)) * skyScatterStrength;
        float3 skyGlow = scatter * TESR_SunColor.rgb * accumLightStrength * strength;
        color = saturate(color + skyGlow);
    }

    return float4(color, 1.0f);
}

float4 CombineLight(VSOUT IN) : COLOR0 {
    float3 color = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord)).rgb;
    float3 volumeLight = linearize(tex2D(TESR_RenderedBuffer, IN.UVCoord)).rgb;
    float3 result = color * (1 - volumeLight) + volumeLight;
    return delinearize(float4(result, 1.0f));
}

technique {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 VolumetricLight();
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 VolumetricLightSky();
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 CombineLight();
    }
}
