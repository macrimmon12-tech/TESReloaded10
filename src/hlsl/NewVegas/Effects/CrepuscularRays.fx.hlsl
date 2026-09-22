// Crepuscular rays (sunbeams through gaps in occluders) for New Vegas Reloaded.
//
// Originally ported from arafuse/tes-reloaded's OblivionReloaded/Shaders/VolumetricLight/
// VolumetricLight.fx.hlsl, then rewritten against this fork's VSM/EVSM cascade shadow atlas
// (near/middle/far/lod, cross-faded) instead of the source's plain near/far depth-compare maps.
//
// Two earlier designs both tried to BRIGHTEN unoccluded pixels (a height-bounded fog-layer march
// that read as flat ground-hugging haze regardless of any real occluder, then a Henyey-Greenstein-
// scattering march whose accumulated light -- driven by TESR_SunColor's real HDR magnitude and a
// glow term that swings by orders of magnitude with view angle toward the sun -- always either
// blew out or was invisible, with no Strength value in between). Both failed for the same
// structural reason: in a typical exterior view, the sun is unoccluded almost everywhere (open
// sky, open ground), so brightening "unoccluded" pixels means brightening most of the screen, and
// any Strength high enough to make the small shadowed minority visible also washes out everything
// else, because that follows from touching the majority, not from any specific tuning value.
//
// This version DARKENS the occluded minority instead, and never touches unoccluded pixels at all.
// The march computes a plain, unweighted-by-color occlusion signal (0 = fully shadowed, 1 = fully
// exposed to the sun -- literally the same average GetSunShadowAmount() debug mode 2 already shows
// working correctly) with no phase function, no sun-color multiplication, no fog-density coupling.
// Composite then multiplies the scene by a factor that stays exactly 1.0 (no change at all)
// wherever that signal reads fully exposed, and only dips below 1.0 -- darkening -- where it reads
// shadowed. An unoccluded pixel is architecturally incapable of being touched, so there is no wash-
// out mechanism left regardless of how high Strength (now "how much to darken") is set.

// The bug that made this produce nothing for most of its development was a camera mismatch,
// not anything in the scattering or the shadow sampling. RenderManager::SetupSceneCamera builds
// the view matrices from *Pointers::Generic::CameraLocation but sets CameraPosition from
// WorldSceneGraph->camera->m_worldTransform.pos. The march took its direction from toWorld(),
// which is built on the first, and its origin from TESR_CameraPosition, which is the second, so
// every marked world position carried the offset between them -- enough to fall outside the
// 212-unit near cascade everywhere, so the shadow lookup returned lit almost always. See
// GetRayOrigin below. Anything here that looks like it is compensating for missing occlusion
// probably is, and should be re-derived rather than trusted.
//
// That offset is also nearly invisible to inspection: an offset position still yields a
// coherent frac() grid, a correct depth image, and correct cascade radii. It was found by
// dumping the lookup's inputs rather than reasoning about its output.

float4 TESR_ReciprocalResolution;
float4 TESR_SmoothedSunDir;
float4 TESR_ShadowFade; // x: sunrise/sunset fade, y: shadow maps active

// xyz unused (were a scatter color tint, dropped along with the phase-function scattering model
// that read it -- see the header comment). w: accum distance cutoff, still the march's range.
float4 TESR_VolumetricLightData1;
// x: darkening strength, consumed only by CompositeLight now, not the march. y, z, w unused (were
// the phase-function sample count, fog influence and anisotropy of the dropped scattering model).
float4 TESR_VolumetricLightData3;
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

// Blue noise for the march start offset, the same texture SunShadows and AmbientOcclusion use.
// WRAP so it tiles one texel per pixel across the half-res buffer.
sampler2D TESR_NoiseSampler : register(s4) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

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

// A cascade's shadow map is an orthographic slab, 2*sphereRadius thick, oriented along the sun.
// Outside it there is no occlusion information at all, and the only correct answer is 'lit'.
//
// Without this test the out-of-range coordinate was handed to the comparison anyway, which does
// not fail gracefully: z > 1 (past the far plane) gives a huge warped depth, so pMax collapses to
// 0 and reads fully SHADOWED, while z < 0 (in front of the near plane) gives a tiny one and reads
// fully LIT. The switch between them is a hard plane in world space -- horizontal a few feet up
// with the sun overhead, tilted with a low sun -- matching no geometry whatever, because it is not
// occlusion. In-game that was the whole of the effect: shafts that cut off at a fixed height and
// corresponded to nothing in view.
//
// It also buried the real signal rather than merely adding to it. Every sample that left the slab
// returned a hard 0 or 1 regardless of what occluders lay along the ray, and with 64 samples those
// dominate the average, so the march reported how much of each ray fell outside the cascade instead
// of what crossed it. Near-field occlusion was being computed correctly and then swamped.
//
// SunShadows.fx.hlsl omits this check safely: its receivers are visible surfaces, and the cascade
// was fitted to the view frustum that contains them, so they are inside the slab by construction.
// A ray march walks thousands of units through open air and leaves it constantly. The Oblivion
// source this was ported from does carry the test (OutsideShadowMap, returning 1.0); it was lost in
// the rewrite onto the cascade atlas. Folded to one max-of-abs as it is there: |x| > 1 is exactly
// x < -1 || x > 1, and |2z - 1| > 1 is exactly z < 0 || z > 1.
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
    // distance to TESR_Shadow*Center as the deferred path does.
    //
    // The distance form was silently selecting nothing. Debug mode 7, which runs those four
    // sphere tests directly and touches neither the atlas nor this function, came back solid
    // black over an entire exterior frame: not one cascade claimed a single pixel. Since this
    // function starts at shadow = 1.0 and only assigns inside one of those tests, it was
    // returning fully lit without ever sampling the shadow map, in every scene, which is why
    // no amount of work on the sampling itself changed anything.
    //
    // Selecting on the projection removes the dependency on those constants entirely: a point
    // is in a cascade exactly when it lands inside that cascade's map, which is the same test
    // already needed to reject out-of-range samples, and is how the Oblivion source this was
    // ported from cascades (GetLightAmount falling through to GetLightAmountFar). It is also
    // strictly more correct -- a bounding sphere overlaps the map it approximates rather than
    // matching it -- at the cost of the smooth cross-fade the radii allowed, which is worth
    // losing to get occlusion at all.
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
// beat the occluders you want shafts from. At 14 samples over a 2000-unit range the step is 143
// units, while a tree trunk's shadow volume is 25-45 units across and a pole's is 15-30: the
// march simply steps over them, landing inside one maybe a fifth of the time, and a single
// darkened sample out of 14 is a 7% dip nobody can see. A building wall's shadow is 300+ units,
// wider than the step, which is why streets produced clear shafts while a forest produced none.
// Dithering the start offset helps beyond the raw count -- neighbouring pixels probe different
// points and the depth-aware upsample averages them -- but it cannot rescue a 5x shortfall.
// Compile-time constant, and an int -- deliberately, not a setting. Driving this from
// TESR_VolumetricLightData3.y made it a runtime bound with a float loop counter, and that
// crashed the D3DX9 HLSL compiler outright: the game died inside CompileEffect with no error
// logged, the log simply ending after this effect registered its texture. ps_3_0's native
// loop instruction counts with an integer register, so a float counter compared against a
// float bound cannot use it, and fxc falls back to a path that does not survive tex2Dlod and
// nested [branch] blocks in the body. Changing the count means editing this line and letting
// the effect recompile; if it needs to be a setting again, the D3DXMACRO route EffectRecord
// already uses for FORWARD_SHADOWS keeps it a compile-time constant, which this must stay.
static const int MARCH_NUM = 64;
static const float NOISE_GRANULARITY = 0.5 / 255.0;

// How much to darken occluded pixels, consumed by CompositeLight -- see its own comment. The
// march itself no longer has a user-facing strength of its own: its job is just to compute a
// clean 0-1 occlusion signal, not to decide how visually strong the final effect is.
static const float strength = TESR_VolumetricLightData3.x;

static const bool ditherEnabled = TESR_VolumetricLightData4.y > 0.5f;
// 0 off, 1 the finished march, 2 the raw shadow term along the ray, 3 that same term at the
// visible surface, 4 the lookup's intermediates, 5-12 the sampling inputs. Mode 2 divides out
// everything layered on top of occlusion -- the distance falloff and Strength -- and shows only
// the average of GetSunShadowAmount along each ray. It answers the one question the finished
// output cannot: whether the cascade lookup finds occluders at all. Mode 1 shows the march's own
// distance-weighted occlusion average after the half-res depth-aware upsample, not composite's
// ad-hoc unweighted one in mode 2 -- worth checking separately to confirm the weighting/upsample
// themselves aren't introducing a wash, distinct from the raw shadow lookup mode 2 tests.
static const float debugMode = TESR_VolumetricLightData4.x;

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

// Low-resolution ray march: walks the view ray from the camera to the visible surface, or to
// the distance-falloff cutoff, whichever is nearer -- sampling the sun shadow atlas at each step
// and accumulating a plain distance-weighted average of it. No phase function, no sun color, no
// fog density: those all multiplied a HDR-magnitude color onto the result, which is exactly what
// made every earlier version either invisible or a wash with nothing usable in between (see the
// header comment). This just answers "how exposed to the sun is the air near the camera along
// this ray," a value that's already naturally in [0,1] with no calibration needed, and leaves
// turning that into a visual effect entirely to CompositeLight's darkening blend.
// Ray origin, taken from TESR_InvViewTransform rather than TESR_CameraPosition.
//
// RenderManager::SetupSceneCamera builds the view and inverse-view matrices from
// *Pointers::Generic::CameraLocation, but sets CameraPosition from
// WorldSceneGraph->camera->m_worldTransform.pos. Those are two different sources for the same
// thing. The march took its direction from toWorld(), which is built on TESR_ViewTransform and
// so on the first of them, while taking its origin from the second -- so any disagreement
// between them offsets every world position the march produces, and with it every cascade test
// and every shadow sample, while still looking perfectly coherent in a frac() grid.
//
// SunShadows.fx.hlsl cannot hit this: reconstructWorldPosition derives position entirely from
// TESR_InvViewTransform, so its origin and direction always agree. Matching that here removes
// the mismatch rather than assuming the two sources are equal.
float3 GetRayOrigin() {
    return float3(TESR_InvViewTransform[3][0], TESR_InvViewTransform[3][1], TESR_InvViewTransform[3][2]);
}

float4 VolumetricLight(VSOUT IN) : COLOR0 {
    float2 uv = IN.UVCoord.xy;
    float3 rayOrigin = GetRayOrigin();

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

    // Blue noise, not the 4x4 ordered pattern this used to use.
    //
    // An ordered pattern repeats every 4 pixels by construction, so the step boundaries it is
    // meant to hide line up into a regular structure instead of scattering. Upsampled from half
    // resolution that became clear diagonal banding with a crosshatch texture across every shaft
    // -- structured, not random, and therefore not something a denoiser can remove: blurring
    // bands only produces softer bands.
    //
    // Blue noise has no such periodicity, so the same step boundaries scatter into fine grain
    // that a blur genuinely does clear. Sampled one texel per pixel, unfiltered, which is how a
    // blue noise mask has to be read to keep its spectral properties.
    float2 noiseUV = uv * (0.5f / TESR_ReciprocalResolution.xy) / 256.0f;
    float ditherOffset = ditherEnabled ? tex2D(TESR_NoiseSampler, noiseUV).r : 0.5f;
    float3 currentPosition = rayOrigin + step * ditherOffset;

    float accumOcclusion = 0.0f;
    float weightSum = 0.0f;
    float accumShadow = 0.0f;

    [loop]
    for (int i = 0; i < MARCH_NUM; i++) {
        // 1.0 where this step sees the sun, towards 0 behind an occluder.
        float Shadow = GetSunShadowAmount(currentPosition);
        accumShadow += Shadow;

        // Per-step distance falloff, not a single value based on the ray's endpoint: a step near
        // the camera should contribute the same whether the ray eventually hits a nearby wall or
        // continues on to open sky far beyond it. Keying the falloff to the ray's endpoint
        // distance instead (an earlier version of this) made the effect only ever visible
        // painted onto whatever solid surface a given ray happened to hit -- since a sky-bound
        // ray's endpoint was always the far cap and got crushed to ~0, while a nearby object's
        // endpoint was always close and got the "full strength" falloff uniformly across its
        // whole silhouette -- and never as a glow genuinely hanging in open air.
        float distFalloff = 1.0f - saturate(distance(currentPosition, rayOrigin) / accumDistance);

        // Weighted average, not a weighted sum: dividing by weightSum (not a fixed MARCH_NUM or
        // accumDistance) keeps this in [0,1] regardless of how much of the march actually carried
        // weight, which is what makes it usable directly as a darkening factor with no separate
        // magnitude calibration the way the old scatterTerm-based accumLight needed.
        accumOcclusion += Shadow * distFalloff;
        weightSum += distFalloff;
        currentPosition += step;
    }
    accumOcclusion /= max(weightSum, 0.0001f);

    // Modes 5-8: the INPUTS to the sampling, not its result. Everything above debugs the
    // shadow lookup while assuming what is fed to it is correct. These check that assumption,
    // because a wrong input produces a plausible-looking wrong output and cannot be told apart
    // from a wrong lookup by staring at the finished image.
    //
    // 5 DEPTH. readDepth(uv) over farZ, greyscale. Expect a smooth near-black to white gradient
    //   with crisp object silhouettes. Flat, banded or inverted means the depth buffer or farZ is
    //   wrong, and then every world position built from it is wrong too.
    [branch] if (debugMode > 4.5f && debugMode < 5.5f)
        return float4(saturate(readDepth(uv) / accumDistance).xxx, 1.0f);

    // 6 WORLD POSITION. frac(pos / 512) as RGB: a world-aligned grid repeating every 512 units.
    //   Expect coloured banding that stays welded to surfaces as the camera moves, and stays put
    //   when only the camera rotates. Swimming with rotation means the reconstruction is wrong.
    [branch] if (debugMode > 5.5f && debugMode < 6.5f)
        return float4(frac((rayOrigin + cameraVector) / 512.0f), 1.0f);

    // 7 CASCADE SELECTION for the surface point. red near, green middle, blue far, yellow lod,
    //   BLACK none. This is the one that matters most: GetSunShadowAmount starts at shadow = 1.0
    //   and only assigns if a cascade claims the point, so anything black here is returning lit
    //   without sampling the atlas at all. Expect concentric bands, red nearest the camera.
    [branch] if (debugMode > 6.5f && debugMode < 7.5f) {
        float3 p = rayOrigin + cameraVector;
        if (length(p - TESR_ShadowNearCenter.xyz)   < TESR_ShadowNearCenter.w)   return float4(1,0,0,1);
        if (length(p - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) return float4(0,1,0,1);
        if (length(p - TESR_ShadowFarCenter.xyz)    < TESR_ShadowFarCenter.w)    return float4(0,0,1,1);
        if (length(p - TESR_ShadowLodCenter.xyz)    < TESR_ShadowLodCenter.w)    return float4(1,1,0,1);
        return float4(0,0,0,1);
    }

    // 9 CASCADE RADII, as one flat colour: TESR_Shadow{Near,Middle,Far}Center.w over 250,
    //   1000 and 3000. Mode 7 came back solid black, meaning not one of the four cascade tests
    //   passed anywhere on screen -- and those tests read these constants directly, without
    //   touching the atlas or GetSunShadowAmount. So either the radii are zero or the centres
    //   are nowhere near the camera, and every theory about precision, slab bounds and atlas
    //   format was downstream of a lookup that never ran. Black here means the constants are
    //   arriving as zero, which is a binding failure rather than anything in the shader maths.
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
    //   surface. This is the test mode 7 should have been. Mode 7 and mode 10 both use the
    //   uncapped surface position, and for a sky pixel that is toWorld * farZ, some 250000
    //   units out and correctly outside every cascade. Their black and saturated results were
    //   therefore consistent with the lookup working, and reading them as proof it never ran
    //   was wrong. March samples are capped at accumDistance from the camera, so unlike the
    //   surface they should land inside a cascade, and anything black here is a real failure.
    //   red near, green middle, blue far, yellow lod, black none.
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
    //   agree and this was never the fault; anything brighter is the offset that was being
    //   applied to every world position the march built.
    [branch] if (debugMode > 11.5f)
        return float4(saturate(length(TESR_CameraPosition.xyz - GetRayOrigin()) / 64.0f).xxx, 1.0f);

    // 8 GATING CONSTANTS, as one flat colour. red TESR_ShadowFade.y (0 disables the lookup
    //   entirely and returns 1.0 before anything is sampled), green shadow mode over 2 so VSM,
    //   EVSM2 and EVSM4 read as 0, 0.5 and 1, blue the format bit where 0 is the 16-bit atlas
    //   whose clamped EVSM exponent costs the depth precision. Any red channel at zero means the
    //   effect cannot produce shadow at all and nothing downstream is worth reading.
    [branch] if (debugMode > 7.5f && debugMode < 8.5f)
        return float4(TESR_ShadowFade.y, ShadowMode * 0.5f, ShadowFormatBits, 1.0f);

    // Mode 4: the lookup's intermediates for the surface point, rather than its verdict.
    // Mode 3 came back white at surface positions too -- the positions SunShadows passes and
    // shadows correctly -- so the fault is not specific to air samples and reading the code has
    // not found it. This shows where each sample actually lands and what it reads there:
    //   RED, GREEN = the near cascade's light-space UV, scaled so the quadrant fills 0..1.
    //                Expect a smooth gradient across the frame. Flat or pinned to an edge means
    //                the transform is wrong and CLAMP is returning the atlas border everywhere.
    //   BLUE       = the moment actually sampled there, scaled by the EVSM positive exponent so
    //                it is visible. Bright blue means a cleared far texel, which reads as lit;
    //                varying blue means real occluder depths are being read and the fault is in
    //                the comparison rather than the addressing.
    [branch] if (debugMode > 3.5f) {
        float3 surfacePos = rayOrigin + cameraVector;
        // The cascade the real lookup would select, not a hardcoded one. Pinning this to Near
        // made the mode useless: Near spans ~212 units, so nearly every visible point projects
        // outside it, saturate clamped to 0 or 1, and the result was flat colour-cube corners that
        // looked like a broken transform while being entirely correct.
        float4x4 sel = TESR_ShadowCameraToLightTransformLod;
        if (length(surfacePos - TESR_ShadowNearCenter.xyz)   < TESR_ShadowNearCenter.w)   sel = TESR_ShadowCameraToLightTransformNear;
        else if (length(surfacePos - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) sel = TESR_ShadowCameraToLightTransformMiddle;
        else if (length(surfacePos - TESR_ShadowFarCenter.xyz)    < TESR_ShadowFarCenter.w)    sel = TESR_ShadowCameraToLightTransformFar;
        float4 lsc = ScreenCoordToTexCoord(mul(float4(surfacePos, 1.0f), sel));
        float2 atlasUV = lsc.xy * 0.5f;
        float moment = SampleShadowMoments(atlasUV).x;
        return float4(saturate(lsc.x), saturate(lsc.y), saturate(moment / exp(5.54f)), 1.0f);
    }

    // Mode 3: the same lookup run at the VISIBLE SURFACE instead of the air samples --
    // exactly the position SunShadows.fx.hlsl feeds it, which is known to shadow correctly.
    // This is the A/B that says whether GetSunShadowAmount is broken outright or only for
    // free-floating points. If mode 3 shows proper shadows and mode 2 does not, the lookup is
    // sound and the fault is specific to air samples -- most likely their light-space depth
    // falling outside the cascade's near/far range, which reads as a cleared (far) texel and so
    // as lit. If mode 3 is white too, the lookup is wrong for every position and differs from
    // SunShadows in some way the two can then be diffed over directly.
    [branch] if (debugMode > 2.5f) {
        float3 surfacePos = rayOrigin + cameraVector;
        return float4(GetSunShadowAmount(surfacePos).xxx, 1.0f);
    }

    // Shadow term on its own, before anything is layered over it -- see debugMode.
    [branch] if (debugMode > 1.5f) return float4((accumShadow / MARCH_NUM).xxx, 1.0f);

    // accumOcclusion is already a proper weighted average (divided by weightSum above, not a
    // fixed sample count or distance), so it's already in [0,1] with no further scaling needed --
    // unlike the old scatterTerm-based accumLight, which needed a path-length normalisation and a
    // hand-calibrated pre-scale to land in a usable range at all. A tiny dither still helps hide
    // quantisation banding in the output once this gets written to the half-res render target.
    accumOcclusion += lerp(-NOISE_GRANULARITY, NOISE_GRANULARITY, rand(uv));

    return float4(saturate(accumOcclusion).xxx, 1.0f);
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

// Full resolution: upsamples the low-res march and darkens the scene with it. No separate
// sky-ambient term: this stays a pure shadow-occlusion shaft effect, not a general atmospheric
// glow -- a pixel only darkens here where the march actually found real occlusion against the sun.
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

    // When every tap is rejected -- a thin feature, or a strong depth discontinuity where all
    // four neighbours sit on the far side of an edge -- dividing by the epsilon floor returns
    // black, not a neutral result. That painted single-pixel black speckle along every
    // silhouette in the frame, and because it rides on top of whatever the march produced it
    // corrupted the diagnostic modes too: debug mode 9 outputs one flat colour with no uv term
    // at all and still came back speckled, which is how it was found. Earlier readings of those
    // specks as genuine occlusion were wrong; they were this.
    //
    // Falling back to the nearest single tap keeps the pixel's own value instead of inventing
    // a black one. It is the right answer as well as a safe one: if no neighbour shares this
    // pixel's depth, the unfiltered sample is exactly what should be used.
    float3 volumeLight = weightSum < 0.0001f
        ? tex2D(TESR_VolumetricLightBuffer, uv).rgb
        : sum / weightSum;

    // Debug view: the march's own output with no scene under it, so what the effect actually
    // computes can be read directly instead of inferred from how it tints the frame. Blown-out
    // highlights and a correctly shaped but over-bright shaft look identical once blended.
    if (debugMode > 0.5f) return float4(volumeLight, 1.0f);

    float3 color = linearize(tex2D(TESR_SourceBuffer, uv)).rgb;

    // Darkens occluded pixels instead of brightening lit ones -- see the header comment for why.
    // volumeLight is a plain occlusion signal (0 = shadowed, 1 = fully exposed to the sun), not a
    // color, so unlike `color` it's used directly rather than delinearized. It's the interpolation
    // factor, not an additive term: at volumeLight == 1, darkenFactor is exactly 1 and the scene
    // is completely untouched, and it only drops below 1 where the march actually found occlusion.
    // Strength sets how deep that darkening goes at full occlusion (0 = effect off, 1 = fully
    // black there). Because an unoccluded pixel is architecturally incapable of being pushed any
    // brighter than it already was, there's no Strength value that can wash out the rest of the
    // frame the way the old additive designs always could -- the majority of a typical view being
    // unoccluded is no longer a liability, since this only ever touches the occluded minority.
    float darkenFactor = lerp(1.0 - saturate(strength), 1.0, saturate(volumeLight.x));
    float3 result = color * darkenFactor;
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
