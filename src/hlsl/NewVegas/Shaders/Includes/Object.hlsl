#if defined(__INTELLISENSE__)
    #include "Pointlights.hlsl"
    #include "PBR.hlsl"
#else
    #include "includes/Pointlights.hlsl"
    #include "includes/PBR.hlsl"
#endif

#if defined(__INTELLISENSE__)
    #include "SkyAmbient.hlsl"
#else
    #include "includes/SkyAmbient.hlsl"
#endif

float4 TESR_PBRData : register(c32);
float4 TESR_PBRExtraData : register(c33);

float getRoughness(float gloss) {
    return saturate(max(0.043, 1 - gloss) * TESR_PBRData.y);
}

float getRoughness(float glossmap, float meshgloss){
    // return pow(glossmap, log(meshgloss));    
    // no gloss = 1
    // full gloss = 0

    return saturate(1 - log(meshgloss) / 4 * glossmap);
    // return 1 - saturate(log(meshgloss)/4 + glossmap);
    // return pow(1 - glossmap, meshgloss);
}

// Vanilla
float3 getVanillaLighting(float3 lightDir, float radius, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float gloss, float glossPower) {
    float att = vanillaAtt(lightDir, radius);
    
    lightDir = normalize(lightDir);
    viewDir = normalize(viewDir);
    float3 halfwayDir = normalize(lightDir + viewDir);
    
    float NdotL = shades(normal.xyz, lightDir.xyz);
    
    #if defined(ONLY_SPECULAR)
        float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
        float3 lighting = saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    #elif defined(SPECULAR)
        float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
        float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
        lighting += saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    #else
        float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
    #endif
    
    return lighting;
}

float3 getVanillaLightingAtt(float3 lightDir, float att, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float gloss, float glossPower) {
    lightDir = normalize(lightDir);
    viewDir = normalize(viewDir);
    float3 halfwayDir = normalize(lightDir + viewDir);
    
    float NdotL = shades(normal.xyz, lightDir.xyz);
    
    #if defined(ONLY_SPECULAR)
        float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
        float3 lighting = saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    #elif defined(SPECULAR)
        float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
        float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
        lighting += saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    #else
        float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
    #endif
    
    return lighting;
}

// PBR
float3 getPointLightLighting(float3 lightDir, float radius, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float roughness) {
    lightColor = lightColor * TESR_PBRData.z;
    albedo = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    
    float att = vanillaAtt(lightDir, radius);
    
    #if defined(ONLY_SPECULAR)
        return att * PBRSpecular(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #elif defined(SPECULAR)
        return att * PBR(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #else
        return att * PBRDiffuse(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #endif
}

float3 getPointLightLightingAtt(float3 lightDir, float att, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float roughness) {
    lightColor = lightColor * TESR_PBRData.z;
    albedo = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    
    #if defined(ONLY_SPECULAR)
        return att * PBRSpecular(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #elif defined(SPECULAR)
        return att * PBR(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #else
    return att * PBRDiffuse(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #endif
}

float3 getSunLighting(float3 lightDir, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float roughness) {
    lightColor = lightColor * TESR_PBRData.z;
    albedo = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    
    #if defined(ONLY_SPECULAR)
        return PBRSunSpecular(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #elif defined(SPECULAR)
        return PBRSun(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #else
        return PBRDiffuse(0, roughness, albedo, normal, viewDir, lightDir, lightColor);
    #endif
}



// [_Main.Develop.Main], via Debug.cpp UpdateSettings. c135: c132 is TESR_ShadowBlur.
// Populated even with Shaders.Debug disabled -- Debug has no per-frame UpdateConstants.
float4 TESR_DebugVar : register(c135);

// --- Hemisphere skylight ------------------------------------------------------------------
// Additive upper-sky term on top of the weather ambient, weighted by w = (1 + N.up) / 2.
// w must stay linear in the dot product: that is the exact cosine-weighted form factor.
// [Shaders.PBR.*] SkylightingScale. No separate toggle: 0 disables the term.
#define SKY_AMBIENT_STRENGTH  (TESR_PBRExtraData.y)      // scale on skyUpper at w = 1
// How far the sky's DIFFUSE irradiance follows the normal map. The reflection is not
// scaled by it -- a reflection off a surface has to follow that surface's detail to mean
// anything, while the diffuse term is a contrast knob for assets authored against a
// renderer that never did it.
#define SKY_AMBIENT_NORMAL    (TESR_PBRExtraData.w)

float3 getAmbientLighting(float3 ambient, float3 albedo) {
    return ambient * TESR_PBRData.w * albedo;
}

// Both halves take SkylightingScale, deliberately, and there is no second knob.
//
// The BRDF is physically based; the radiance feeding it is not. GetSkyColor interpolates
// weather-authored display colours and scales them by artistic settings, so it carries no
// radiometric unit -- there is one unknown constant between "the colour the sky dome is painted"
// and "the radiance arriving at a surface", and SkylightingScale is it. That constant is a
// property of the light source, so it is the same number for both halves.
//
// Splitting it in two would let the diffuse and specular responses of one light source be dialled
// against each other, which nothing physical can do: how the sky's energy divides between the two
// is the BRDF's answer, not a setting.

// What the surface reflects of the sky.
//
// This must be added where the TEXTURE_Vc multiply cannot reach it. An ONLY_LIGHT pass lights a
// white surface and has its whole output multiplied by the texture afterwards, which is right
// for the diffuse ambient and wrong for a reflection: reflectance already carries the albedo
// through f0, so passing through that multiply applies it a second time. So the reflection
// belongs to the passes nothing multiplies -- the self-contained combined ones, and the additive
// ONLY_SPECULAR ones.
//
// metallicness is 0 at every call site on this branch. Nothing supplies a per-material metal
// value yet, and the global Metallicness setting cannot stand in for one here: a split
// ONLY_SPECULAR pass has no diffuse texture bound, so f0 would pick up a white albedo there and
// the real albedo in the combined pass, and the two decompositions would disagree on any
// surface the engine happened to split. At 0 the term is a flat dielectric 0.04 and the
// disagreement cannot arise. The parameter is here because that is the only piece missing.
//
// worldView points from the surface toward the camera. The carried shadow world position is
// camera-relative, so normalising its negation gives it with no extra interpolator.
float3 getSkyReflection(float3 albedo, float3 worldNormal, float worldNormalValid,
                        float3 worldView, float roughness, float metallicness) {
    float3 f0 = lerp(float(0.04).rrr, albedo, metallicness);
    return SkyAmbientSpecular(worldNormal, worldView, roughness, f0, TESR_PBRExtraData.z)
         * SKY_AMBIENT_STRENGTH * worldNormalValid;
}

// Ambient for the permutations that carry a view vector.
//
// Metallicness splits the ambient rather than the reflection being added on top: a metal has no
// diffuse response, so its share moves to getSkyReflection instead of being counted twice.
float3 getAmbientLighting(float3 ambient, float3 albedo, float3 worldNormal, float worldNormalValid,
                          float3 worldView, float roughness, float metallicness,
                          float3 mappedNormal) {
    float3 flatAmbient = ambient * TESR_PBRData.w;

    // The sky is an environment light, so its irradiance belongs at the shading normal -- which
    // is what the direct sun and the point lights already use, through tangent-space N.L. The
    // geometric normal was the odd term out, and it left surfaces flat in shade, where the sky
    // ambient is the whole of the lighting. Order-2 SH is a heavy low-pass, but irradiance stays
    // near-linear in the normal away from the pole, so this is not a subtle change: integrating
    // a uniform sky against the band factors, a 30 degree normal-map slope moves a vertical
    // surface by +/-50%. A floor barely moves, which is the cosine being flat at its peak.
    //
    // AmbientScale (TESR_PBRData.w) scales the weather ambient above but not this: the sky is a
    // second, independent light source, so SkylightingScale is its only strength knob and it
    // survives AmbientScale = 0.
    float3 diffuseNormal = BlendShadingNormal(worldNormal, mappedNormal, SKY_AMBIENT_NORMAL);
    float3 skyTerm = SkyAmbientRadiance(diffuseNormal, TESR_PBRExtraData.z) * SKY_AMBIENT_STRENGTH;

    // worldNormalValid is 0 under a vanilla VS, where the carried world position is undefined.
    float3 diffuse = (flatAmbient + skyTerm * worldNormalValid) * albedo * (1.0f - metallicness);

#if !defined(SPECULAR)
    // The material renders no specular lobe -- vanilla draws no highlight for it at all -- so
    // there is nothing for a reflection to belong to, the same rule the terrain PBR branch
    // follows. The metallic split is dropped with it: moving a metal's share out of the diffuse
    // is only right when something adds it back, and here nothing would.
    return (flatAmbient + skyTerm * worldNormalValid) * albedo;
#elif defined(ONLY_LIGHT)
    // Has a lobe, but this pass is multiplied by the texture afterwards, so the reflection
    // cannot ride along. The split decomposition adds it from its ONLY_SPECULAR pass instead,
    // which is also what makes the metallic split above correct here.
    return diffuse;
#else
    return diffuse + getSkyReflection(albedo, mappedNormal, worldNormalValid,
                                      worldView, roughness, metallicness);
#endif
}
