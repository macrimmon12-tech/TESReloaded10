// Sky-colour ambient for the lighting shaders (objects, parallax, terrain, skin, hair, grass,
// decals). The shaders reach it through SkyAmbientRedistribute at the bottom of this file, which
// uses the sky to redistribute the weather ambient by orientation rather than adding light.
//
// Two models, selected at COMPILE time by [Shaders.PBR.Main] SkylightingMode. ps_3_0 flattens
// runtime branches, so a switch here would make every lit pixel pay for both paths; the macro
// comes through ShaderRecord::LoadShader the same way FORWARD_SHADOWS does, and changing the
// setting alters the preprocessed source so the shader cache recompiles.
//
//   0 -- spherical harmonic irradiance (default)
//   1 -- single directional sample, the older model
//
// Both return a GAMMA-ENCODED value. The object and terrain shaders carry no gamma conversion
// anywhere, so their ambient is gamma-encoded, and SKY.pso delinearises GetSkyColor before
// writing it. Keeping both modes in that space means switching between them compares the two
// MODELS rather than confounding the comparison with a colour-space difference.
//
// The encode is sqrt(), i.e. gamma 2.0, not the sRGB curve. One instruction per channel against
// a pow, and the few percent it differs from sRGB is far smaller than the error it replaces:
// integrating encoded values instead of encoding the result cost walls about a third of their
// brightness. Both modes use the same encode so the comparison stays honest.
#ifndef SKYAMBIENT_INCLUDED
#define SKYAMBIENT_INCLUDED

#ifndef SKYLIGHTING_MODE
    #define SKYLIGHTING_MODE 0
#endif

#if SKYLIGHTING_MODE == 1

// ---------------------------------------------------------------------------
// Mode 1: one sample of the sky, in a direction leaning from straight up toward the surface
// normal, scaled by a hand-written cosine form factor.
//
// Known shortcomings, kept deliberately so the two can be compared:
//   - lerp(up, N, d) is `up` for ANY d when N is up, so directionality does nothing at all on
//     floors and terrain -- they are pinned to the zenith colour.
//   - a point sample cannot represent a hemisphere: the cosine-weighted density peaks 45
//     degrees off the normal, so the horizon band is under-weighted.
//   - the horizon falloff has to be softened below SKY.pso's own 8, or sunInfluence -- which
//     only reaches the output through terms scaled by athmosphere -- vanishes at the lifted
//     sample direction.
// ---------------------------------------------------------------------------
float4 TESR_SkyColor     : register(c137);
float4 TESR_SkyLowColor  : register(c138);
float4 TESR_HorizonColor : register(c139);
float4 TESR_SunPosition  : register(c140);
float4 TESR_SkyData      : register(c141); // x: atmosphere thickness, y: sun influence, z: sun strength
float4 TESR_SunAmount    : register(c142); // x: dayTime
float4 TESR_SunDiskColor : register(c143);
float4 TESR_SunsetColor  : register(c144);

// Helpers first: Sky.hlsl uses linearize(), and several shaders include PBRScale (and so this
// file) before their own Helpers include.
#if defined(__INTELLISENSE__)
    #include "Helpers.hlsl"
    #include "Sky.hlsl"
#else
    #include "includes/Helpers.hlsl"
    #include "includes/Sky.hlsl"
#endif

#ifndef SKY_AMBIENT_ATMOSPHERE_POW
    #define SKY_AMBIENT_ATMOSPHERE_POW 2
#endif

// Mirrors SKY.pso's derivation with the sample direction substituted for its eye ray.
float3 GetSkyRadiance(float3 dir) {
    float3 up = float3(0.0f, 0.0f, 1.0f);

    float verticality = pows(compress(dot(dir, up)), 3);
    float sunHeight = shade(TESR_SunPosition.xyz, up);
    float athmosphere = pows(1 - verticality, SKY_AMBIENT_ATMOSPHERE_POW) * TESR_SkyData.x;
    float sunInfluence = pows(compress(dot(dir, TESR_SunPosition.xyz)), 1 / TESR_SkyData.y);

    float3 sunColor = GetSunColor(sunHeight, TESR_SkyData.x, TESR_SunAmount.x,
                                  TESR_SunDiskColor.rgb, TESR_SunsetColor.rgb);

    return GetSkyColor(verticality, athmosphere, sunHeight, sunInfluence, TESR_SkyData.z,
                       TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, sunColor);
}

// directionality: [Shaders.PBR.*] / [Shaders.Terrain.*] SkylightingDirectionality.
float3 SkyAmbientDirection(float3 worldNormal, float directionality) {
    float3 up = float3(0.0f, 0.0f, 1.0f);

    // lerp(up, N, d) is the ZERO vector when N points straight down and d is exactly 0.5, and
    // ceilings and undersides have precisely that normal. normalize() of it is NaN.
    float3 v = lerp(up, worldNormal, directionality);
    float len = length(v);
    return (len > 1e-4f) ? (v / len) : up;
}

float3 SkyAmbientRadiance(float3 worldNormal, float directionality) {
    float3 dir = SkyAmbientDirection(worldNormal, directionality);

    // Cosine-weighted form factor for the visible hemisphere: 1 facing up, 0 facing down.
    float wSky = 0.5f * worldNormal.z + 0.5f;

    // GetSkyColor returns linear; encode to match the space the callers work in.
    return sqrt(max(GetSkyRadiance(dir), 0.0f)) * wSky;
}

// The same sample, left linear for SkyAmbientRedistribute, with the form factor applied to the
// radiance rather than to its encode.
float3 SkyIrradianceLinear(float3 worldNormal, float directionality) {
    float3 dir = SkyAmbientDirection(worldNormal, directionality);
    return max(GetSkyRadiance(dir), 0.0f) * (0.5f * worldNormal.z + 0.5f);
}

// Average over every orientation. This model has no closed form for it, so it takes what a dome
// of uniform radiance gives: half of what an upward facing surface receives. That costs this mode
// a second sky evaluation per pixel; mode 0 reads its average off a coefficient.
float3 SkyIrradianceMeanLinear() {
    return 0.5f * max(GetSkyRadiance(float3(0.0f, 0.0f, 1.0f)), 0.0f);
}

// This mode evaluates the sky from its colour constants directly, so it is always available.
float SkyAmbientAvailable() {
    return 1.0f;
}

#else

// ---------------------------------------------------------------------------
// Mode 0: order-2 spherical harmonic irradiance.
//
// SkyShaders::UpdateConstants projects the sky -- the same GetSkyColor that SKY.pso renders the
// dome with -- onto 9 coefficients once per frame and convolves them with the clamped-cosine
// kernel, so this evaluates
//
//     E(N) = INTEGRAL L(w) max(N.w, 0) dw
//
// rather than approximating it by a sample direction. The cosine kernel is a severe low-pass
// filter, so 9 coefficients carry that integral to within about 1% (Ramamoorthi & Hanrahan
// 2001).
//
// The projection runs on LINEAR radiance, which is what the integral is defined on; the
// reconstruction below is encoded.
//
// The cosine form factor, the wall/floor split and the sun-side azimuthal bias are all inherent
// to the convolution. There is no direction to lean, so `directionality` is ignored here.
// ---------------------------------------------------------------------------
float4 TESR_SkyIrradiance[9] : register(c137);

// worldNormal must be the GEOMETRIC world normal, unit length. The reconstruction is LINEAR
// irradiance.
float3 SkyIrradianceLinear(float3 worldNormal, float directionality) {
    float3 n = worldNormal;

    return TESR_SkyIrradiance[0].rgb
         + TESR_SkyIrradiance[1].rgb * n.y
         + TESR_SkyIrradiance[2].rgb * n.z
         + TESR_SkyIrradiance[3].rgb * n.x
         + TESR_SkyIrradiance[4].rgb * (n.x * n.y)
         + TESR_SkyIrradiance[5].rgb * (n.y * n.z)
         + TESR_SkyIrradiance[6].rgb * (3.0f * n.z * n.z - 1.0f)
         + TESR_SkyIrradiance[7].rgb * (n.x * n.z)
         + TESR_SkyIrradiance[8].rgb * (n.x * n.x - n.y * n.y);
}

// Average over every orientation. Every basis function past the first integrates to zero over
// the sphere, (3z^2 - 1) included since z^2 averages 1/3, so the average is the constant term.
float3 SkyIrradianceMeanLinear() {
    return TESR_SkyIrradiance[0].rgb;
}

// Sky.cpp sets [0].w to 1 once it has computed the coefficients, which it only does while the
// Sky shader is enabled. Until then they hold nothing a surface should be lit by.
float SkyAmbientAvailable() {
    return saturate(TESR_SkyIrradiance[0].w);
}

// Encoded. max() first because an order-2 SH fit can ring slightly negative, and sqrt of a
// negative is NaN.
float3 SkyAmbientRadiance(float3 worldNormal, float directionality) {
    return sqrt(max(SkyIrradianceLinear(worldNormal, directionality), 0.0f));
}

#endif

// ---------------------------------------------------------------------------
// The sky REDISTRIBUTES the weather ambient. It does not add to it.
//
// The weather ambient is one colour arriving at every surface whichever way it faces. The sky is
// not: a surface facing up sees the whole dome, one facing sideways half dome and half ground,
// one facing down the ground. Both models above describe the upper half only, so the ground is
// taken to send back the flat ambient itself - the light the weather says arrives from
// everywhere, which for a downward facing surface is very nearly what the ground reflects. For a
// uniform lower hemisphere the cosine integral of that is flatAmbient * (1 - N.z) / 2.
//
// The sum is then rescaled so that its average over every orientation carries the flat
// ambient's luminance. Raising the strength therefore moves light between orientations - floors
// toward the sky, undersides toward the ground, the sun's side of a wall toward the warmer part
// of the dome - and cannot raise that average. So the setting needs no brightness control of its
// own to undo one it introduced, and AmbientScale stays the one brightness control for all of the
// ambient, sky included.
//
// flatAmbient arrives encoded and already scaled by AmbientScale. The sums run on linear values,
// which is what they are defined on (see the header); the encode is sqrt, so the decode is the
// square.
//
// valid is 0 where the carried world position is undefined, i.e. under a vanilla vertex shader.
// The orientation is unknown there, so it takes the orientation-free average rather than the flat
// ambient: that gives up the direction and keeps the sky's colour, where falling back to the flat
// ambient would let two touching surfaces disagree about what colour the light is.
//
// strength: [Shaders.PBR.*] / [Shaders.Terrain.*] SkylightingScale, 0 to 1. 0 is the flat
// weather ambient exactly, and so is any strength while the sky has not been computed.
// ---------------------------------------------------------------------------
float3 SkyAmbientRedistribute(float3 flatAmbient, float3 worldNormal, float directionality, float strength, float valid) {
    const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

    float3 groundLin = flatAmbient * flatAmbient;
    float3 meanLin = SkyIrradianceMeanLinear() + 0.5f * groundLin;

    // A select, not a multiply: the normal is undefined where valid is 0, and 0 * NaN is NaN.
    float3 dirLin = (valid > 0.5f)
        ? SkyIrradianceLinear(worldNormal, directionality) + groundLin * (0.5f - 0.5f * worldNormal.z)
        : meanLin;

    float rescale = dot(groundLin, lumaWeights) / max(dot(meanLin, lumaWeights), 1e-6f);
    return lerp(flatAmbient, sqrt(max(dirLin * rescale, 0.0f)), saturate(strength) * SkyAmbientAvailable());
}

#endif
