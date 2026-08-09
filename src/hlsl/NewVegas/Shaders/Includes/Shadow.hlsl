// Forward sun-shadow lookup for GAME shaders (objects, parallax, terrain).
//
// All four cascades live in one 2x2 atlas (TESR_ShadowAtlas).
//
// The maths here mirrors Effects/SunShadows.fx.hlsl so that the forward and deferred paths
// produce the same value for the same point. If you change the cascade selection, bias, or
// bleed-reduction constants in one, change them in the other.
//
// SPACE CONVENTION
//   TESR_ShadowCameraToLightTransform* consume CAMERA-RELATIVE WORLD space (i.e.
//   reconstructWorldPosition(), which does NOT add TESR_CameraPosition). Game object shaders
//   are model-space, so the caller must reconstruct it -- see GetShadowWorldPos(), intended
//   for use in the vertex shader.
//
// USAGE
//   VS:  OUT.shadowWorldPos = GetShadowWorldPos(OUT.sPosition);
//   PS:  sunLight *= GetSunShadow(IN.shadowWorldPos, worldNormal);
//   Both are compiled out entirely when FORWARD_SHADOWS is 0.

// ---------------------------------------------------------------------------
// Constants. Bound by name via ShaderRecord::CreateCT; the register assignments below do not
// affect binding, only where the values live.
//
// EVERY constant here must stay pinned to an explicit register. Left unpinned, the compiler
// packs them into the gaps between the host shader's declared constants, and those gaps are
// not free -- they are registers the engine still writes for the vanilla shader, which NVR's
// replacement simply doesn't declare. The engine then overwrites them mid-frame and cascade
// selection reads garbage.
//
// c100+ is clear: the heaviest host is TerrainTemplate's pixel shader at c93, and the skinned
// vertex shader's Bones[54] ends at c97. ps_3_0 allows 224 float constants and vs_3_0 allows
// 256, so c100-c133 is inside both.
//
// The two matrices the VERTEX side needs are relocatable, because c100+ is not universally
// safe in a vertex shader: GRASS23x00*.vso indexes InstanceData relatively from c20 and the
// batch can run far past c100.
// ---------------------------------------------------------------------------
#ifndef SHADOW_INVPROJ_REG
    #define SHADOW_INVPROJ_REG c100
#endif
#ifndef SHADOW_INVVIEW_REG
    #define SHADOW_INVVIEW_REG c104
#endif

row_major float4x4 TESR_InvProjectionTransform : register(SHADOW_INVPROJ_REG);
row_major float4x4 TESR_InvViewTransform       : register(SHADOW_INVVIEW_REG);

row_major float4x4 TESR_ShadowCameraToLightTransformNear   : register(c108);
row_major float4x4 TESR_ShadowCameraToLightTransformMiddle : register(c112);
row_major float4x4 TESR_ShadowCameraToLightTransformFar    : register(c116);
row_major float4x4 TESR_ShadowCameraToLightTransformLod    : register(c120);

float4 TESR_ShadowNearCenter   : register(c124); // xyz: centre (camera-relative world), w: radius
float4 TESR_ShadowMiddleCenter : register(c125);
float4 TESR_ShadowFarCenter    : register(c126);
float4 TESR_ShadowLodCenter    : register(c127);

float4 TESR_ShadowData        : register(c128); // y: darkness (z is INTERIOR cube texel size only)
float4 TESR_ShadowFormatData  : register(c129); // x: mode (0 VSM, 1 EVSM2, 2 EVSM4), y: format bits
float4 TESR_ShadowFade        : register(c130); // x: sunrise/sunset fade, y: shadow maps active
float4 TESR_SmoothedSunDir    : register(c131);
float4 TESR_ShadowBlur        : register(c132); // x: 1 / atlas resolution, y: lod cascade updated
float4 TESR_ShadowForwardData : register(c133); // x: 1 when the forward path is SUPPRESSED

// s9 suits the object templates, which top out at s7. Templates with wider sampler arrays
// must override this BEFORE including: TerrainTemplate has NormalMap[7] spanning s7-s13.
#ifndef SHADOW_ATLAS_SAMPLER_REG
    #define SHADOW_ATLAS_SAMPLER_REG s9
#endif

sampler2D TESR_ShadowAtlas : register(SHADOW_ATLAS_SAMPLER_REG) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP;
    MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE;
};

// Atlas encoding, fixed at compile time. The deferred path can afford to branch on
// TESR_ShadowFormatData.x at runtime because it runs once per screen pixel; this runs per
// fragment with overdraw, and ps_3_0 would flatten the branch across all three VSM/EVSM
// variants.
//
// Must match [_Shaders.ShadowsExteriors.ShadowMaps] Mode in the toml.
//   0 = VSM, 1 = EVSM2, 2 = EVSM4 (toml default)
#ifndef SHADOW_FIXED_MODE
    #define SHADOW_FIXED_MODE 2
#endif

static const float SHADOW_FORMAT = TESR_ShadowFormatData.y;

// Normal-offset bias, in SHADOW MAP TEXELS -- not world units.
//
// A texel's world size is 2*radius/cascadeResolution, and the cascade radii span two orders of
// magnitude: at Distance 6000 / CascadeLambda 0.95 / CascadeResolution 2048 the Near cascade
// texel is roughly 0.15 world units and the Lod cascade texel roughly 3. A fixed world-space
// offset is therefore several texels up close (peter-panning) and a fraction of one at range,
// well below the one texel needed to escape the quantised depth of the receiving surface.
// Expressed in texels the offset is scale-invariant and correct in every cascade.
//
// It has to clear the prefilter kernel as well as the texel grid: ShadowMapBlur.pso reaches
// +/-3.23 texels on the near cascade (Blur9) and +/-1.33 on the others (Blur5), so a filtered
// moment describes a neighbourhood several texels wide.
#ifndef SHADOW_NORMAL_BIAS_TEXELS
    #define SHADOW_NORMAL_BIAS_TEXELS 2.5f
#endif

// Extra widening of the VSM/EVSM minimum variance at grazing incidence. The Chebyshev bound
// needs more slack where a single texel spans a long run of depth across the receiver, which
// is precisely the grazing case. 0 disables it and restores a constant bias.
#ifndef SHADOW_SLOPE_BIAS
    #define SHADOW_SLOPE_BIAS 1.0f
#endif

// Lookup-time filtering, in taps per cascade, on top of the Gaussian prefilter. 1 matches the
// deferred path. 4 costs 12 extra texture fetches per shaded fragment.
//
// Above 1, average the MOMENTS and run Chebyshev once, as SampleShadowMoments does. Averaging
// independent Chebyshev results is not equivalent -- the bound is not linear in the moments.
#ifndef SHADOW_FILTER_TAPS
    #define SHADOW_FILTER_TAPS 1
#endif

// Tap spacing in atlas texels.
#ifndef SHADOW_FILTER_SPREAD
    #define SHADOW_FILTER_SPREAD 1.0f
#endif

// Vertex-shader presence sentinel.
//
// NVR replaces far more PIXEL shaders than VERTEX shaders -- for objects roughly 134 vs 51,
// for terrain 29 vs 2 -- and the game pairs the two independently by lighting configuration,
// so an NVR pixel shader frequently runs against a VANILLA vertex shader. Vanilla does not
// write the camera-relative world position this path needs, leaving that interpolator holding
// undefined data.
//
// NVR's vertex shaders stamp this sentinel alongside the position; the pixel shader checks it
// and declines to shadow rather than reading garbage.
#define SHADOW_VS_SENTINEL 1.0f
#define SHADOW_VS_PRESENT(w) (abs((w) - SHADOW_VS_SENTINEL) < 0.001f)

// Master switch, injected as a D3DXMACRO by ShaderRecord::LoadShader from the
// [Shaders.ShadowsExteriors.Main] ForwardShadows setting, the same way REVERSED_DEPTH is.
// Governs whether the forward path is compiled in at all; TESR_ShadowForwardData.x governs
// whether it runs. Defaults to 0 so these shaders are stock behaviour if nothing injects it.
#ifndef FORWARD_SHADOWS
    #define FORWARD_SHADOWS 0
#endif

// ---------------------------------------------------------------------------
// VSM / EVSM evaluation. Mirrors Effects/Includes/Shadows.hlsl.
// Duplicated rather than cross-included: the Effects and Shaders trees are compiled
// independently, and that file also pulls in point-light cube helpers this path has no use for.
// ---------------------------------------------------------------------------
float ShadowLinstep(float a, float b, float v) {
    return saturate((v - a) / (b - a));
}

float ShadowReduceLightBleeding(float pMax, float amount) {
    return ShadowLinstep(amount, 1.0f, pMax);
}

float ShadowChebyshevUpperBound(float2 moments, float mean, float minVariance, float bleedReduction) {
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);

    float d = mean - moments.x;
    float pMax = variance / (variance + (d * d));

    pMax = ShadowReduceLightBleeding(pMax, bleedReduction);

    return (mean <= moments.x ? 1.0f : pMax);
}

float2 ShadowEVSMExponents() {
    const float maxExponent = (SHADOW_FORMAT == 0.0f) ? 5.54f : 42.0f;
    return min(float2(40.0f, 5.0f), maxExponent);
}

float2 ShadowWarpDepth(float depth, float2 exponents) {
    depth = 2.0f * depth - 1.0f;
    return float2(exp(exponents.x * depth), -exp(-exponents.y * depth));
}

// ---------------------------------------------------------------------------
// Atlas sampling. The four cascades occupy the four quadrants of one texture:
//   Near (0.0, 0.0)   Middle (0.5, 0.0)
//   Far  (0.0, 0.5)   Lod    (0.5, 0.5)
// ---------------------------------------------------------------------------
// tex2Dlod, not tex2D: the cascade selection below is a dynamic branch, and a gradient-taking
// sample inside one is illegal in ps_3_0 (error X3528). The atlas has no mipmaps
// (ShadowMaps.Mipmaps = false), so an explicit LOD 0 is exactly equivalent.
float4 SampleShadowMoments(float2 uv, float2 quadrantOffset) {
#if SHADOW_FILTER_TAPS <= 1
    return tex2Dlod(TESR_ShadowAtlas, float4(uv, 0.0f, 0.0f));
#else
    // Every tap must stay inside its own quadrant. The atlas packs four unrelated cascades
    // into one texture, so a tap that crosses a quadrant border reads another cascade's depths
    // as if they belonged to this one. Half a texel of inset keeps bilinear off the seam.
    float texel = TESR_ShadowBlur.x;
    float2 lo = quadrantOffset + texel * 0.5f;
    float2 hi = quadrantOffset + 0.5f - texel * 0.5f;

    // Rotated grid: four taps on the diagonals cover the texel footprint more evenly than four
    // on the axes, for the same cost.
    float2 d = texel * SHADOW_FILTER_SPREAD;
    float4 m;
    m  = tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + d * float2( 1.0f,  0.5f), lo, hi), 0.0f, 0.0f));
    m += tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + d * float2(-0.5f,  1.0f), lo, hi), 0.0f, 0.0f));
    m += tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + d * float2(-1.0f, -0.5f), lo, hi), 0.0f, 0.0f));
    m += tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + d * float2( 0.5f, -1.0f), lo, hi), 0.0f, 0.0f));
    return m * 0.25f;
#endif
}

float SampleShadowAtlas(float4 lightSpace, float2 quadrantOffset, float bias, float bleedReduction) {
    lightSpace.xyz /= lightSpace.w;
    lightSpace.x = lightSpace.x * 0.5f + 0.5f;
    lightSpace.y = lightSpace.y * -0.5f + 0.5f;

    // Fold into the correct atlas quadrant.
    lightSpace.xy = lightSpace.xy * 0.5f + quadrantOffset;

    float4 moments = SampleShadowMoments(lightSpace.xy, quadrantOffset);

#if SHADOW_FIXED_MODE == 0
    // VSM
    return ShadowChebyshevUpperBound(moments.xy, lightSpace.z, bias, bleedReduction);
#elif SHADOW_FIXED_MODE == 1
    // EVSM2
    float2 exponents = ShadowEVSMExponents();
    float2 warped = ShadowWarpDepth(lightSpace.z, exponents);
    float2 depthScale = bias * exponents * warped;
    return ShadowChebyshevUpperBound(moments.xy, warped.x, depthScale.x * depthScale.x, bleedReduction);
#else
    // EVSM4
    float2 exponents = ShadowEVSMExponents();
    float2 warped = ShadowWarpDepth(lightSpace.z, exponents);
    float2 depthScale = bias * exponents * warped;
    float2 minVariance = depthScale * depthScale;

    float posContrib = ShadowChebyshevUpperBound(moments.xz, warped.x, minVariance.x, bleedReduction);
    float negContrib = ShadowChebyshevUpperBound(moments.yw, warped.y, minVariance.y, bleedReduction);
    return min(posContrib, negContrib);
#endif
}

// ---------------------------------------------------------------------------
// Camera-relative world position from a clip-space position.
// Call in the VERTEX shader and interpolate the result: perspective-correct interpolation of
// world position is exact, and this costs one interpolator rather than four.
// ---------------------------------------------------------------------------
float3 GetShadowWorldPos(float4 clipPos) {
    float4 viewPos = mul(clipPos, TESR_InvProjectionTransform);
    viewPos /= viewPos.w;
    return mul(viewPos, TESR_InvViewTransform).xyz;
}

// ---------------------------------------------------------------------------
// Geometric world normal, from screen-space derivatives of the interpolated world position.
//
// Object shaders carry their per-pixel normal in TANGENT space (lights are pushed through the
// TBN in the vertex shader), so there is no world normal available to hand to the bias, and
// passing one would cost three floats the MAX_LIGHTS > 4 permutation has no room for. Deriving
// it costs no interpolators and works in every permutation.
//
// This is the flat face normal rather than the normal-mapped one, which is what the offset
// bias wants: the bias is about escaping the depth of the surface the fragment sits on, not
// its shading detail.
//
// MUST be called from the top level of the pixel shader -- ddx/ddy are gradient instructions
// and are illegal inside dynamic flow control.
// ---------------------------------------------------------------------------
float3 GetShadowGeometricNormal(float3 worldPos) {
    float3 n = normalize(cross(ddx(worldPos), ddy(worldPos)));

    // worldPos is camera-relative, so the camera sits at the origin: the outward-facing normal
    // is the one pointing back towards it. Removes any winding/handedness doubt.
    return n * sign(dot(n, -worldPos));
}

// ---------------------------------------------------------------------------
// Main entry point. Returns 1.0 in full light, falling towards 0 in full shadow.
// Multiply the SUN term by this, before ambient is added.
// ---------------------------------------------------------------------------
float GetSunShadow(float3 worldPos, float3 worldNormal) {
    // Forward path suppressed at runtime. SunShadows.fx picks the cascades up deferred in the
    // same frame, reading the same constant, so exactly one of the two ever applies shadows.
    //
    // Note the sense of the test: nonzero means SUPPRESSED. A constant that fails to arrive
    // reads as zero and leaves the forward path running, which is the safe failure mode.
    if (TESR_ShadowForwardData.x) return 1.0f;

    // Shadow maps switched off entirely (setting), or sun below horizon.
    if (!TESR_ShadowFade.y) return 1.0f;

    // Atlas encoding guard. ShadowMap.pso writes a different channel layout per [ShadowMaps]
    // Mode -- raw moments for VSM, exponentially warped pairs for EVSM2, those plus their
    // squares for EVSM4. Decoding with the wrong one produces garbage rather than a subtle
    // difference, so fail cleanly (unshadowed) instead.
    if (TESR_ShadowFormatData.x != (float)SHADOW_FIXED_MODE) return 1.0f;

    // Normal offset: push the sample point along the surface normal, scaled by how grazing the
    // sun is.
    float NdotL = dot(worldNormal, TESR_SmoothedSunDir.xyz);
    float offsetScale = saturate(1.0f - NdotL);

    float4 radii = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w,
                          TESR_ShadowFarCenter.w,  TESR_ShadowLodCenter.w);

    // World size of one shadow map texel, per cascade.
    //
    // GetCascadeViewProj builds each cascade's ortho box as [-sphereRadius, +sphereRadius] in
    // x and y, so the box is 2*radius across and one texel is 2*radius/cascadeResolution.
    // cascadeResolution is not published to shaders, but TESR_ShadowBlur.x is the reciprocal of
    // the ATLAS resolution and the atlas is exactly two cascades wide:
    //     cascadeResolution = 0.5 / TESR_ShadowBlur.x
    //     texelWorldSize    = 4 * radius * TESR_ShadowBlur.x
    // The floor guards against the constant arriving as zero, keeping the offset small rather
    // than infinite.
    float4 texelWorld = 4.0f * radii * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
    float4 offsetDistance = offsetScale * SHADOW_NORMAL_BIAS_TEXELS * texelWorld;

#if SHADOW_FIXED_MODE == 0
    const float baseBias = 0.00001f;
#else
    const float baseBias = 0.01f;
#endif

    // Slope-scaled variance floor. bias drives minVariance, the slack the Chebyshev bound is
    // allowed before it calls a texel occluded. A grazing texel spans a long run of receiver
    // depth and so needs more of it.
    float bias = baseBias * (1.0f + SHADOW_SLOPE_BIAS * offsetScale);

    // Fraction of the way to a cascade's border at which the cross-fade into the next begins.
    const float blend = 0.9f;

    // Each cascade gets its OWN offset, in its own texel scale. A single shared sample position
    // cannot suit all four when their texels differ by more than an order of magnitude: an
    // offset that clears the Lod cascade's 3-unit texels would lift the sample some 20 texels
    // off the surface in the Near cascade.
    float4 shadows = float4(
        SampleShadowAtlas(mul(float4(worldPos + offsetDistance.x * worldNormal, 1.0f), TESR_ShadowCameraToLightTransformNear),   float2(0.0f, 0.0f), bias, 0.1f),
        SampleShadowAtlas(mul(float4(worldPos + offsetDistance.y * worldNormal, 1.0f), TESR_ShadowCameraToLightTransformMiddle), float2(0.5f, 0.0f), bias, 0.2f),
        SampleShadowAtlas(mul(float4(worldPos + offsetDistance.z * worldNormal, 1.0f), TESR_ShadowCameraToLightTransformFar),    float2(0.0f, 0.5f), bias, 0.6f),
        SampleShadowAtlas(mul(float4(worldPos + offsetDistance.w * worldNormal, 1.0f), TESR_ShadowCameraToLightTransformLod),    float2(0.5f, 0.5f), bias, 0.8f));

    // Select by distance to each cascade's centre, cross-fading over the outer 10%. Circular
    // boundaries, matching SunShadows.fx -- the light-space NDC box is square, so selecting on
    // it instead puts visible square-edged steps on the ground normal to the sun.
    float4 distances = float4(
        length(worldPos - TESR_ShadowNearCenter.xyz),
        length(worldPos - TESR_ShadowMiddleCenter.xyz),
        length(worldPos - TESR_ShadowFarCenter.xyz),
        length(worldPos - TESR_ShadowLodCenter.xyz));

    // Initialised, not merely assigned in every branch: a point beyond the last cascade falls
    // through all of them.
    float shadow = 1.0f;
    if (distances.x < radii.x) {
        shadow = (distances.x < radii.x * blend) ? shadows.x
               : lerp(shadows.x, shadows.y, smoothstep(radii.x * blend, radii.x, distances.x));
    }
    else if (distances.y < radii.y) {
        shadow = (distances.y < radii.y * blend) ? shadows.y
               : lerp(shadows.y, shadows.z, smoothstep(radii.y * blend, radii.y, distances.y));
    }
    else if (distances.z < radii.z) {
        shadow = (distances.z < radii.z * blend) ? shadows.z
               : lerp(shadows.z, shadows.w, smoothstep(radii.z * blend, radii.z, distances.z));
    }
    else if (distances.w < radii.w) {
        shadow = lerp(shadows.w, 1.0f, smoothstep(radii.w * blend, radii.w, distances.w));
    }

    shadow = saturate(shadow);

    // Fade out as the sun approaches the horizon, matching SunShadows.fx.
    shadow = lerp(shadow, 1.0f, saturate(TESR_ShadowFade.x));

    return shadow;
}
