#if defined(__INTELLISENSE__)
    #include "Helpers.hlsl"
    #include "Pointlights.hlsl"
    #include "PBR.hlsl"
#else
    #include "includes/Pointlights.hlsl"
    #include "includes/PBR.hlsl"
#endif

#if defined(__INTELLISENSE__)
    #include "SkyAmbient.hlsl"
    #include "TerrainVariation.hlsl"
#else
    #include "includes/SkyAmbient.hlsl"
    #include "includes/TerrainVariation.hlsl"
#endif

// Stochastic tiling. Off unless ShaderRecord.cpp defines it, and even then only the first
// TERRAIN_VARIATION_LAYERS blend layers take the two-tap path -- see TerrainVariation.hlsl.
#ifndef TERRAIN_VARIATION
    #define TERRAIN_VARIATION 0
#endif
#ifndef TERRAIN_VARIATION_LAYERS
    #define TERRAIN_VARIATION_LAYERS 1
#endif

float4 TESR_TerrainData : register(c89);
float4 TESR_TerrainExtraData : register(c90);

// Hemisphere skylight, mirroring Includes/Object.hlsl. Terrain never includes that file -- it
// carries its own getVanillaLightingAtt -- so the constants are declared here. c134/c135 are
// clear on this side: the terrain chain tops out at c92 and Shadow.hlsl pins c100-c133.
// [Shaders.Terrain.*] SkylightingScale in .x. Its own constant because TESR_TerrainExtraData is
// full: x usePBR, y saturation, z NoiseScale, w NoiseTile.
float4 TESR_TerrainSkyData : register(c135);

// Additive upper-sky term on top of the weather ambient, weighted by w = (1 + N.up) / 2.
// w must stay linear in the dot product: that is the exact cosine-weighted form factor.
// No separate toggle: a strength of 0 disables it.
#ifndef SKY_AMBIENT_STRENGTH
    #define SKY_AMBIENT_STRENGTH  (TESR_TerrainSkyData.x)    // scale on skyUpper at w = 1
#endif

// stochasticWeights carries each layer's albedo blend weight out to blendNormalMaps, so that
// layer's normal comes from the same mix of the two taps its colour did. One entry per layer
// rather than one shared float: the weight is height-dependent, so it differs per texture.
// dx/dy are the original uv derivatives, hoisted to top level by the caller.
//
// The i < TERRAIN_VARIATION_LAYERS test is on the index of an [unroll]ed loop, so fxc folds it
// and layers past the limit keep their single tex2D untouched. The array is only ever indexed
// by that same unrolled i, so it stays in registers -- no relative addressing.
float3 blendDiffuseMaps(float3 vertexColor, float2 uv, float2 dx, float2 dy, int texCount, sampler2D tex[7], float blends[7], StochasticOffsets stochastic, inout float stochasticWeights[7]) {
    float3 color = float3(0, 0, 0);

    [unroll] for (int i = 0; i < texCount; i++) {
#if TERRAIN_VARIATION
        if (i < TERRAIN_VARIATION_LAYERS) {
            color += StochasticSampleAlbedo(tex[i], uv, dx, dy, stochastic, stochasticWeights[i]).xyz * blends[i];
        }
        else {
            color += tex2D(tex[i], uv).xyz * blends[i];
        }
#else
        color += tex2D(tex[i], uv).xyz * blends[i];
#endif
    }

    return color * vertexColor;
}

float3 blendNormalMaps(float2 uv, float2 dx, float2 dy, int texCount, sampler2D tex[7], float blends[7], float spec[7], StochasticOffsets stochastic, float stochasticWeights[7], out float gloss, out float specExponent) {
    gloss = 0.0f;
    specExponent = 0.0f;

    float3 blendedNormal = float3(0, 0, 0);

    float4 normal;
    [unroll] for (int i = 0; i < texCount; i++) {
#if TERRAIN_VARIATION
        if (i < TERRAIN_VARIATION_LAYERS) {
            normal = StochasticSampleWith(tex[i], uv, dx, dy, stochastic, stochasticWeights[i]);
        }
        else {
            normal = tex2D(tex[i], uv);
        }
#else
        normal = tex2D(tex[i], uv);
#endif
        blendedNormal += normal.xyz * blends[i];
        gloss += normal.w * blends[i] * (spec[i] > 0 ? 1.0f : 0.0f);
        specExponent += spec[i] * blends[i];
    }

    gloss = saturate(gloss);
    return normalize(expand(blendedNormal));
}

float3 getVanillaLightingAtt(float3 lightDir, float att, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float gloss, float glossPower) {
    lightDir = normalize(lightDir);
    viewDir = normalize(viewDir);
    float3 halfwayDir = normalize(lightDir + viewDir);
    
    float NdotL = shades(normal.xyz, lightDir.xyz);
    
    float specStrength = gloss * pow(abs(shades(normal.xyz, halfwayDir.xyz)), glossPower);
    float3 lighting = albedo.rgb * NdotL * lightColor.rgb * att;
    lighting += saturate(((0.2 >= NdotL ? (specStrength * saturate(NdotL + 0.5)) : specStrength) * lightColor.rgb) * att);
    
    return lighting;
}

float3 getPointLightLighting(float3 lightDir, float att, float3 lightColor, float3 eyeDir, float3 normal, float3 albedo, float gloss = 0.0, float glossPower = 0.0, float metallicness = 1.0) {
    float3 pointlightColor = lightColor * TESR_TerrainData.z;

    [branch]
    if (TESR_TerrainExtraData.x){
        // PBR. 
        float roughness = saturate((1 - gloss) * TESR_TerrainData.y);
        float3 lighting = PBR(saturate(metallicness * TESR_TerrainData.x), roughness, albedo, normal, eyeDir, lightDir, pointlightColor);
        
        return max(0, lighting * att);
    } else {
        // Vanilla.    
        lightDir = normalize(lightDir);
        
        float3 lighting = getVanillaLightingAtt(lightDir, att, lightColor, eyeDir, normal, albedo, gloss, glossPower);
        
        return lighting;
    }
}

float3 getSunLighting(float3 lightDir, float3 sunColor, float3 eyeDir, float3 normal, float3 AmbientColor, float3 albedo, float gloss = 0.0, float glossPower = 0.0, float metallicness = 1.0, float parallaxMultiplier = 1.0, float3 worldNormal = float3(0.0f, 0.0f, 1.0f)) {
    float3 lightColor = sunColor * TESR_TerrainData.z * parallaxMultiplier;
    float3 ambientColor = AmbientColor * TESR_TerrainData.w;

    // Hemisphere skylight, matching getAmbientLighting in Object.hlsl. worldNormal is the
    // geometric world normal, not the tangent-space shading normal used above. AmbientScale
    // (TESR_TerrainData.w) scales the weather ambient above but not this: the sky is a second,
    // independent light source, so SkylightingScale is its only strength knob and it survives
    // AmbientScale = 0.
    ambientColor += SkyAmbientRadiance(worldNormal, TESR_TerrainSkyData.y) * SKY_AMBIENT_STRENGTH;
    float3 color = albedo;
    color = lerp(luma(albedo), color, TESR_TerrainExtraData.y);

    [branch]
    if (TESR_TerrainExtraData.x) {
        // PBR.
        float roughness = saturate((1 - gloss) * TESR_TerrainData.y);
        float3 lighting = PBRSun(saturate(metallicness * TESR_TerrainData.x), roughness, color, normal, eyeDir, lightDir, lightColor);
        return max(0, lighting + ambientColor * color);
    } else {
        // Vanilla, no specular.
        float3 lighting = getVanillaLightingAtt(lightDir, 1.0, sunColor, eyeDir, normal, albedo, gloss, glossPower);
        
        return lighting + ambientColor * color;
    }
}
