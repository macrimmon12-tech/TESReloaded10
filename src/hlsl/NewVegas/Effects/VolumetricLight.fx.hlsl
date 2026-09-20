// Volumetric Light shafts for New Vegas Reloaded.
//
// Originally ported from arafuse/tes-reloaded's OblivionReloaded/Shaders/VolumetricLight/
// VolumetricLight.fx.hlsl, then rewritten against this fork's VSM/EVSM cascade shadow atlas
// (near/middle/far/lod, cross-faded) instead of the source's plain near/far depth-compare maps.
//
// This is now a from-scratch design for NVR rather than a port: two in-game tests (screenshots)
// showed the source shader's height-bounded "fog layer" ray march (march within a volume capped
// by a HEIGHT setting, animated flow-noise density, wind scroll) reads as a flat ground-hugging
// haze, not shafts -- a fully-lit ray inside that fog volume still glows even with zero occluder
// in view, so it looks like ambient fog rather than light breaking through gaps. In a second test,
// its unclamped accumulated light blew past 1.0 in a debris-dense area (lots of small sky gaps),
// which inverted CompositeLight's blend and erased the scene under a flat wash entirely.
//
// So: no fog volume, no height ceiling, no animated noise, no wind. The march is just camera to
// visible surface (or the falloff cutoff, whichever is nearer), testing the sun shadow atlas at each
// step. The only thing that ever brightens a pixel is GetSunShadowAmount() varying along the ray
// -- i.e. an actual occluder -- and the final output is saturated once, at the source, so it can
// never invert the composite blend. General atmospheric haze is VolumetricFog.fx.hlsl's job, not
// this one's.

float4 TESR_ReciprocalResolution;
float4 TESR_SmoothedSunDir;
float4 TESR_SunColor;
float4 TESR_ShadowFade; // x: sunrise/sunset fade, y: shadow maps active

float4 TESR_VolumetricLightData1; // xyz: scatter color tint, w: accum distance cutoff
float4 TESR_VolumetricLightData3; // x: strength (intensity multiplier), w: anisotropy (y, z unused)
float4 TESR_VolumetricLightData4; // x: debug view toggle, y: dither toggle (z, w unused)

// Two techniques, not two passes of one technique: EffectRecord::Render() keeps a single
// RenderTarget/RenderedSurface pair for every pass of one Render() call, so a low-res march and
// a full-res composite can't share a technique. Technique 0 (March) renders into its own
// dedicated half-res TESR_VolumetricLightBuffer via RenderEffectToRT, same pattern as
// FlashlightBeamEffect's TESR_VolumetricBuffer. Technique 1 (Composite) runs later, at full
// resolution, through the normal RenderEffectsPreTonemapping chain.
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
sampler2D TESR_VolumetricLightBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Shadows.hlsl"

// --- Cascade shadow lookup -----------------------------------------------------------------
// Mirrors SunShadows.fx.hlsl's GetLightAmount (same registered constants, same atlas), copied
// rather than shared: the two effects compile independently and SunShadows.fx.hlsl already
// documents this "mirrored, not shared" pattern for the game-shader/effect split. Keep the two
// in step if cascade selection, bias, or atlas layout ever change.
// No row_major here: unlike the game-shader path (Shaders/Includes/Shadow.hlsl), which binds
// matrices by raw register index and needs it explicit, D3DX Effects bind by name through
// Effect->SetMatrix() with their own default (non-row_major) packing -- SunShadows.fx.hlsl,
// the other Effect consuming these same matrices, declares them plain for exactly this reason.
// row_major here silently transposed every light-space transform, which is why no occluder was
// ever detected: a transposed transform sends samples outside the valid [0,1] shadow-map region
// for virtually every real point, and CLAMP addressing on TESR_ShadowAtlas turns "off the edge"
// into "reads as unoccluded" almost universally.
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

float GetShadowValue(float4x4 lightTransform, float4 coord, float offsetX, float offsetY, float bias, float bleedReduction) {
    float4 lightSpaceCoord = ScreenCoordToTexCoord(mul(coord, lightTransform));
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

// 1.0 in full light, towards 0 in shadow. positionWS is an absolute world position -- the same
// space SunShadows.fx.hlsl feeds these transforms from reconstructWorldPosition(), and the space
// TESR_Shadow*Center's xyz are expressed in, so the caller folds TESR_CameraPosition in first.
//
// No normal-offset bias here, unlike the game-shader/deferred versions this mirrors: that bias
// exists to push a SURFACE sample away from itself to fight self-shadowing acne, and only makes
// sense with a real geometric normal. The march samples free-floating points in open air, not a
// surface, so there is no meaningful normal to offset along -- a flat bias is the correct choice,
// not a degenerate substitute (an earlier version of this passed the sun direction itself as
// "normal", which made NdotL always exactly 1 and silently zeroed the bias everywhere).
float GetSunShadowAmount(float3 positionWS) {
    if (!TESR_ShadowFade.y) return 1.0f;

    const float bias = ShadowMode == 0.0f ? 0.00001f : 0.01f;
    const float blend = 0.9f;

    float4 radii = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w);
    float4 distances = float4(
        length(positionWS - TESR_ShadowNearCenter.xyz),
        length(positionWS - TESR_ShadowMiddleCenter.xyz),
        length(positionWS - TESR_ShadowFarCenter.xyz),
        length(positionWS - TESR_ShadowLodCenter.xyz));

#define VL_SHADOW_TAP_NEAR   GetShadowValue(TESR_ShadowCameraToLightTransformNear,   float4(positionWS, 1.0f), 0.0f, 0.0f, bias, 0.1f)
#define VL_SHADOW_TAP_MIDDLE GetShadowValue(TESR_ShadowCameraToLightTransformMiddle, float4(positionWS, 1.0f), 0.5f, 0.0f, bias, 0.2f)
#define VL_SHADOW_TAP_FAR    GetShadowValue(TESR_ShadowCameraToLightTransformFar,    float4(positionWS, 1.0f), 0.0f, 0.5f, bias, 0.6f)
#define VL_SHADOW_TAP_LOD    GetShadowValue(TESR_ShadowCameraToLightTransformLod,    float4(positionWS, 1.0f), 0.5f, 0.5f, bias, 0.8f)

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

    // Deliberately NOT faded by TESR_ShadowFade.x here, unlike every surface-shading consumer
    // of these same cascades. That fade ramps to a full 1.0 (shadows entirely off) for any
    // dayLight in [0.4, 0.6] -- see ShadowsExterior.cpp, smoothStep(0.5, 0.1, abs(dayLight-0.5)),
    // whose bounds run backwards -- and stays partially faded across the rest of each sunrise and
    // sunset ramp, reaching 0 only once dayLight pins at 1.0 (or 0.0). It exists to hide
    // shadow acne on SURFACES at grazing sun angles, where a cascade texel spans a long run of
    // receiver depth. The march samples free-floating points in open air: there is no surface to
    // self-shadow, so there is no acne to hide, and the fade buys nothing.
    //
    // Applying it here forced this function to return exactly 1.0 on every sample of every ray
    // at precisely the hours god rays exist for, collapsing the march to scatterTerm*distFalloff
    // -- a uniform, geometry-independent haze. The atlas is still rendered and valid throughout
    // (ShouldRenderShadowMaps never consults ShadowFade.x), so reading it here is sound.
    // ShadowFade.y, the master "shadow maps active" toggle, is still honoured at the top.
    return saturate(shadow);
}

// --- Ray march setup -------------------------------------------------------------------------

static const float4x4 DITHER_PATTERN = { 0.0f, 0.5f, 0.125f, 0.625f, 0.75f, 0.22f, 0.875f, 0.375f, 0.1875f, 0.6875f, 0.0625f, 0.5625f, 0.9375f, 0.4375f, 0.8125f, 0.3125f };

static const int MARCH_NUM = 14;
static const float NOISE_GRANULARITY = 0.5 / 255.0;

static const float strength = TESR_VolumetricLightData3.x;
static const float anisotropy = TESR_VolumetricLightData3.w;
static const bool ditherEnabled = TESR_VolumetricLightData4.y > 0.5f;
static const bool debugView = TESR_VolumetricLightData4.x > 0.5f;

// Every uniform-wash screenshot so far shares one signature: the sky (correctly suppressed by
// the distance falloff) looks fine while everything nearby is a flat, undifferentiated plateau
// -- not literally inverted (that bug is already fixed by the saturate() below), just genuinely
// hitting 1.0 and staying there regardless of shadow state, which erases whatever contrast the
// shadow value would otherwise produce. TESR_SunColor carries real HDR magnitude in this engine
// (this is the same PBR pipeline ObjectTemplate.hlsl's PBRSun/PBRDiffuse consume, not a display-
// range [0,1] color), so a fully-lit ray was almost certainly clipping well before the shadow
// term ever got a chance to pull it back down. Cut hard from the source shader's 3.0 so a
// fully-lit ray has headroom below 1.0 for the shadow value to actually carve a visible gap out
// of, and treat Strength (the user-facing setting) as the knob to raise from here, not this.
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
    VSOUT OUT = (VSOUT) 0.0f;
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
// (1-g^2)/(4*PI*(1-g)^3), which climbs from 0.108 at g=0.1 to 0.796 at g=0.6 -- so tightening
// the lobe also made the whole effect 7.4x brighter, far past where the composite blend
// saturates, and the frame washed out to a flat sheet that destroyed the very structure the
// tighter lobe was meant to reveal. Dividing through by that peak decouples the two knobs:
// Anisotropy controls how sharply light gathers toward the sun, Strength controls how much
// there is. The peak term is a closed form -- at lightDotView == 1 the denominator is
// (1 + g^2 - 2g)^1.5 = ((1-g)^2)^1.5 = (1-g)^3 -- so this costs no extra evaluation.
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
float4 VolumetricLight(VSOUT IN) : COLOR0 {
    float2 uv = IN.UVCoord.xy;

    float depth = readDepth(uv);
    bool isSky = depth > (farZ * 0.99f);

    float3 cameraVector = toWorld(uv) * depth;
    float3 rayDirection = normalize(cameraVector);

    // March only as far as the distance falloff below actually weights: every sample past
    // accumDistance is multiplied by zero, so spending steps out there buys nothing and costs
    // the resolution of the part that does count. Marching to the visible surface (or 20000
    // units into open sky) instead put a sky ray's step length at 20000/14 = ~1430 units, so
    // only its first two or three samples landed inside the falloff window at all -- the whole
    // open-sky result was reconstructed from three samples 1430 units apart, while a ray that
    // hit a wall 700 units out got all 14 at 50-unit spacing. That quality cliff between
    // "ray hit something near" and "ray reached sky" is what read as soft blobs of light
    // hanging in mid-air, untethered from any geometry.
    float accumDistance = max(TESR_VolumetricLightData1.w, 1.0f);
    float rayLength = isSky ? accumDistance : min(length(cameraVector), accumDistance);
    float3 step = rayDirection * (rayLength / MARCH_NUM);

    // 0.5/Reciprocal, not 1/Reciprocal: TESR_ReciprocalResolution describes the full-res back
    // buffer, but this technique renders into the half-res TESR_VolumetricLightBuffer, so uv
    // spans the half-res target. Scaling by the full-res size advanced the index by two per
    // pixel, so only two of the pattern's four columns (and rows) were ever reachable -- a 2x2
    // dither doing a 4x4 dither's job, which leaves banding for the bilinear upsample to smear.
    float2 ditherPixel = abs(uv) * 0.5f / TESR_ReciprocalResolution.xy;
    float ditherOffset = ditherEnabled
        ? DITHER_PATTERN[int(ditherPixel.x) % 4][int(ditherPixel.y) % 4]
        : 0.5f;
    float3 currentPosition = TESR_CameraPosition.xyz + step * ditherOffset;

    float lightDotView = dot(rayDirection, TESR_SmoothedSunDir.xyz);
    float3 lightColor = TESR_VolumetricLightData1.xyz * TESR_SunColor.rgb;
    float scatterCeiling = isSky ? 1.0f : 0.5f;
    float3 scatterTerm = ComputeScattering(lightDotView, scatterCeiling).xxx * lightColor;

    float3 accumLight = 0.0f.xxx;

    [loop]
    for (int i = 0; i < MARCH_NUM; i++) {
        // 1.0 where this step sees the sun, towards 0 behind an occluder.
        float Shadow = GetSunShadowAmount(currentPosition);

        // Per-step distance falloff, not a single value based on the ray's endpoint: a step near
        // the camera should contribute the same whether the ray eventually hits a nearby wall or
        // continues on to open sky far beyond it. Keying the falloff to the ray's endpoint
        // distance instead (an earlier version of this) made the effect only ever visible
        // painted onto whatever solid surface a given ray happened to hit -- since a sky-bound
        // ray's endpoint was always the far cap and got crushed to ~0, while a nearby object's
        // endpoint was always close and got the "full strength" falloff uniformly across its
        // whole silhouette -- and never as a glow genuinely hanging in open air.
        float distFalloff = 1.0f - saturate(distance(currentPosition, TESR_CameraPosition.xyz) / accumDistance);

        accumLight += scatterTerm * Shadow * distFalloff;
        currentPosition += step;
    }

    // Mean sample value, then back to a path integral: the physical quantity is the integral of
    // scattered light along the ray, sum(f) * stepLength, and stepLength is rayLength/MARCH_NUM.
    // Dividing by MARCH_NUM alone yields a mean with NO dependence on how far the ray actually
    // travelled, so 20 units of air in front of a near wall accumulated exactly as much light as
    // 4000 units of open sky -- every surface in the frame got the same wash regardless of how
    // much air was really in front of it, which is what read as haze paint on nearby geometry
    // rather than depth. Normalised by accumDistance so a full-length ray keeps the magnitude
    // this was calibrated at and Strength stays meaningful.
    accumLight *= rayLength / (accumDistance * MARCH_NUM);
    accumLight *= accumLightStrength * strength;
    accumLight += lerp(-NOISE_GRANULARITY, NOISE_GRANULARITY, rand(uv));

    // Saturated here, once, at the source: CompositeLight's blend (color*(1-v)+v) only behaves
    // as a blend for v in [0,1] -- above that it inverts and swamps the scene color entirely,
    // which is exactly the "everything erased to a flat wash" failure an earlier, unclamped
    // version of this produced in debris-dense areas with lots of small sky gaps.
    return float4(saturate(accumLight), 1.0f);
}

// One tap of the depth-aware upsample. exp2 falls off fast enough that a tap on the far side
// of a silhouette contributes essentially nothing, while the depth difference across a
// continuous surface (even a steeply raked one) stays well inside the kernel. Relative to
// centerDepth, not absolute: the same slope spans a far larger absolute depth range at 4000
// units than at 40, and an absolute tolerance would either bleed up close or over-reject far off.
void AccumulateTap(float2 tapUV, float centerDepth, inout float3 sum, inout float weightSum) {
    float tapDepth = readDepth(tapUV);
    float weight = exp2(-32.0f * abs(tapDepth - centerDepth) / max(centerDepth, 1.0f));
    sum += tex2D(TESR_VolumetricLightBuffer, tapUV).rgb * weight;
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
// side of the edge -- a halo of light detached from the object that should bound it. Scaling the
// march by path length widens precisely that near/far gap, so this matters more now, not less.
float4 CompositeLight(VSOUT IN) : COLOR0 {
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

    // Guarded: on a thin feature every tap can land on a different surface and drop out.
    float3 volumeLight = sum / max(weightSum, 0.0001f);

    // Debug view: the march's own output with no scene under it, so what the effect actually
    // computes can be read directly instead of inferred from how it tints the frame. Blown-out
    // highlights and a correctly shaped but over-bright shaft look identical once blended.
    if (debugView) return float4(volumeLight, 1.0f);

    float3 color = linearize(tex2D(TESR_SourceBuffer, uv)).rgb;
    volumeLight = linearize(volumeLight);
    float3 result = color * (1 - volumeLight) + volumeLight;
    return delinearize(float4(result, 1.0f));
}

technique March {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 VolumetricLight();
    }
}

technique Composite {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 CompositeLight();
    }
}
