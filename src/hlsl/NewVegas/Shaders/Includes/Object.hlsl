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
// The weather ambient, redistributed by orientation: see SkyAmbientRedistribute. [Shaders.PBR.*]
// SkylightingScale, 0 to 1. 0 is the flat weather ambient; no value changes the ambient's
// average over orientations, which stays AmbientScale's.
#define SKY_AMBIENT_STRENGTH  (TESR_PBRExtraData.y)

// [Shaders.PBR.*] SkylightingNormalStrength: how far the sky's DIFFUSE irradiance follows the
// normal map. 1 is the shading normal, 0 the flat geometric one. The reflection always follows the
// normal map: a reflection that ignored the surface's detail would describe a different surface.
#define SKY_AMBIENT_NORMAL    (TESR_PBRExtraData.w)

float3 getAmbientLighting(float3 ambient, float3 albedo) {
    return ambient * TESR_PBRData.w * albedo;
}

// What the surface reflects of the sky and the ground (SkyAmbientSpecular), as a dielectric: f0
// 0.04. Nothing supplies a per-material metal value, and the global Metallicness setting cannot
// stand in for one here - a split ONLY_SPECULAR pass has no diffuse texture bound, so an
// albedo-tinted f0 would differ between the split and the combined decompositions of one surface.
//
// It must be added where the texture multiply of an ONLY_LIGHT pass cannot reach it: that pass
// lights a white surface and has its whole output multiplied by the texture afterwards, which is
// right for the diffuse ambient and would tint a reflection by the albedo. So it goes to the
// passes nothing multiplies - the combined ones, and the ONLY_SPECULAR ones - and non-POINT only,
// so exactly one pass of a split decomposition adds it.
//
// worldView points from the surface toward the camera; the carried world position is
// camera-relative, so it is that position's negation, normalised.
float3 getSkyReflection(float3 mappedNormal, float worldNormalValid, float3 worldView, float roughness) {
    return SkyAmbientSpecular(TESR_SunAmbient.rgb * TESR_PBRData.w, mappedNormal, worldView, roughness,
                              float(0.04f).xxx, SKY_AMBIENT_STRENGTH, worldNormalValid);
}

float3 getAmbientLighting(float3 ambient, float3 albedo, float3 worldNormal, float worldNormalValid,
                          float3 mappedNormal, float3 worldView, float roughness) {
    float3 flatAmbient = ambient * TESR_PBRData.w;

    // The sky is an environment light, so its irradiance belongs at the shading normal -- which is
    // what the direct sun and the point lights already use, through tangent-space N.L. At the
    // geometric normal it left surfaces flat in shade, where the ambient is the whole of the
    // lighting.
    float3 diffuseNormal = BlendShadingNormal(worldNormal, mappedNormal, SKY_AMBIENT_NORMAL);

    // worldNormalValid is 0 under a vanilla VS, where the carried world position is undefined.
    float3 diffuse = SkyAmbientRedistribute(flatAmbient, diffuseNormal, TESR_PBRExtraData.z, SKY_AMBIENT_STRENGTH, worldNormalValid) * albedo;

#if defined(SPECULAR) && !defined(ONLY_LIGHT)
    return diffuse + getSkyReflection(mappedNormal, worldNormalValid, worldView, roughness);
#else
    // No specular lobe - vanilla draws no highlight for this material, so there is nothing for a
    // reflection to belong to - or an ONLY_LIGHT pass, whose ONLY_SPECULAR partner adds it.
    return diffuse;
#endif
}
