// PBR light/ambient scales for the hand-ported game shaders (skin, hair, grass).
// Object.hlsl applies the same .z and .w in getSunLighting / getPointLightLighting /
// getAmbientLighting.
//
// c134, past Shadow.hlsl's c100-c133. Object.hlsl must not be included here: it declares
// TESR_PBRData at c32.
//
// DEPENDENCY: ShaderManager::UpdateConstants refreshes PBRShaders unconditionally. A zero
// scale renders everything below black.
#ifndef PBRSCALE_INCLUDED
#define PBRSCALE_INCLUDED

#if defined(__INTELLISENSE__)
    #include "SkyAmbient.hlsl"
#else
    #include "includes/SkyAmbient.hlsl"
#endif

float4 TESR_PBRData : register(c134);

// rgb only: skin carries flags in AmbientColor.a and PSLightColor.a.
float3 PBRLight(float3 lightColor) { return lightColor * TESR_PBRData.z; }
float4 PBRLight(float4 lightColor) { return float4(lightColor.rgb * TESR_PBRData.z, lightColor.a); }

float3 PBRAmbient(float3 ambient) { return ambient * TESR_PBRData.w; }
float4 PBRAmbient(float4 ambient) { return float4(ambient.rgb * TESR_PBRData.w, ambient.a); }

// Hemisphere skylight, matching getAmbientLighting in Object.hlsl. Strength is
// [Shaders.PBR.*] SkylightingScale; there is no toggle, 0 disables it.
//
// Registers above TESR_PBRData's c134. Every shader including this file tops out well below
// c100, and Shadow.hlsl pins c100-c133.
float4 TESR_PBRExtraData : register(c135);

// worldNormal must be the GEOMETRIC world normal, not a tangent-space shading normal --
// GetShadowGeometricNormal gives it. valid is 0 where the carried world position is undefined,
// i.e. under a vanilla vertex shader. w = (1 + N.up) / 2 stays linear in the dot product: that
// is the exact cosine-weighted form factor.
//
// AmbientScale (TESR_PBRData.w) is deliberately absent: this is a second, independent light
// source rather than a tint on the weather ambient, so SkylightingScale is its only strength
// knob and it survives AmbientScale = 0.
float3 SkyAmbient(float3 worldNormal, float valid) {
    return SkyAmbientRadiance(worldNormal, TESR_PBRExtraData.z) * TESR_PBRExtraData.y * valid;
}

// With a normal map. The sky is an environment light, so its irradiance belongs at the shading
// normal -- the same normal the direct sun already uses. TESR_PBRExtraData.w dials it back for
// assets authored against a renderer that never did this; the reflection is not scaled by it.
float3 SkyAmbient(float3 worldNormal, float valid, float3 mappedNormal) {
    float3 n = BlendShadingNormal(worldNormal, mappedNormal, TESR_PBRExtraData.w);
    return SkyAmbientRadiance(n, TESR_PBRExtraData.z) * TESR_PBRExtraData.y * valid;
}

// The specular half of the same sky, for the hand-ported shaders that render a specular lobe.
//
// Additive, and deliberately outside whatever albedo multiply the caller does: reflectance
// already carries the albedo through f0, so passing this through one applies it twice.
//
// worldView points from the surface toward the camera; the carried shadow world position is
// camera-relative, so normalising its negation gives it with no extra interpolator.
float3 SkyReflection(float3 albedo, float3 worldNormal, float valid, float3 worldView,
                     float roughness, float metallicness) {
    float3 f0 = lerp(float(0.04).rrr, albedo, metallicness);
    return SkyAmbientSpecular(worldNormal, worldView, roughness, f0, TESR_PBRExtraData.z)
         * TESR_PBRExtraData.y * valid;
}

#endif
