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
#else
    #include "includes/SkyAmbient.hlsl"
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

float3 blendDiffuseMaps(float3 vertexColor, float2 uv, int texCount, sampler2D tex[7], float blends[7]) {
    float3 color = float3(0, 0, 0);
    [unroll] for (int i = 0; i < texCount; i++) {
        color += tex2D(tex[i], uv).xyz * blends[i];
    }

    return color * vertexColor;
}

// [Shaders.Terrain.*] SkylightingNormalStrength: how far the sky's DIFFUSE follows the ground's
// normal maps. The reflection is not scaled by it -- a reflection has to follow the surface it
// comes off to mean anything.
#define SKY_AMBIENT_NORMAL  (TESR_TerrainSkyData.z)

// Rotate an object-space normal into world space.
//
// Terrain's vertex tangent frame, its LOD normal maps, SunDir (c18) and EyePosition (c16) are
// all object-space -- the engine transforms light and eye per draw, the Gamebryo convention --
// while the sky's spherical harmonics are indexed by a world direction with +Z up. The two
// coincide only if the land block carries no rotation. That is likely and is not worth
// assuming.
//
// Both positions are already interpolated, so screen-space derivatives give two independent
// vectors in each space, and for a rigid transform that pins the rotation exactly. The
// world-space pair is already being differentiated by GetShadowGeometricNormal, so the new cost
// is the object-space pair and a few crosses. On an unrotated block this comes out as the
// identity by itself.
//
// The rotation is constant across the object, so a smooth objN stays smooth. That is the reason
// for doing it this way rather than building a frame around the geometric normal, which is
// per-triangle constant and would facet visibly across terrain's large triangles -- the sky
// terms are faceted today for exactly that reason.
//
// Gradients: call from pixel-shader top level only.
float3 ObjectToWorldNormal(float3 objN, float3 objPos, float3 worldPos, float3 fallback) {
    float3 do1 = ddx(objPos),   do2 = ddy(objPos);
    float3 dw1 = ddx(worldPos), dw2 = ddy(worldPos);

    float3 oz = cross(do1, do2);
    float3 wz = cross(dw1, dw2);

    // A silhouette pixel or a collapsed quad leaves one of these at zero, and normalize() of
    // zero is NaN. The geometric normal the caller already holds is the right fallback.
    if (dot(do1, do1) < 1e-12f || dot(oz, oz) < 1e-12f || dot(wz, wz) < 1e-12f) return fallback;

    float3 ox = normalize(do1);
    oz = normalize(oz);
    float3 oy = cross(oz, ox);

    float3 wx = normalize(dw1);
    wz = normalize(wz);
    float3 wy = cross(wz, wx);

    // R maps object to world, and cross() commutes with a rotation, so the two frames built the
    // same way from the same screen derivatives differ by exactly R. Applied as W * O^T without
    // ever forming R.
    return normalize(wx * dot(ox, objN) + wy * dot(oy, objN) + wz * dot(oz, objN));
}

float3 blendNormalMaps(float2 uv, int texCount, sampler2D tex[7], float blends[7], float spec[7], out float gloss, out float specExponent) {
    gloss = 0.0f;
    specExponent = 0.0f;

    float3 blendedNormal = float3(0, 0, 0);

    float4 normal;
    [unroll] for (int i = 0; i < texCount; i++) {
        normal = tex2D(tex[i], uv);
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

float3 getSunLighting(float3 lightDir, float3 sunColor, float3 eyeDir, float3 normal, float3 AmbientColor, float3 albedo, float gloss = 0.0, float glossPower = 0.0, float metallicness = 1.0, float parallaxMultiplier = 1.0, float3 worldNormal = float3(0.0f, 0.0f, 1.0f), float3 worldView = float3(0.0f, 0.0f, 1.0f), float3 worldShadingNormal = float3(0.0f, 0.0f, 1.0f)) {
    float3 lightColor = sunColor * TESR_TerrainData.z * parallaxMultiplier;
    float3 ambientColor = AmbientColor * TESR_TerrainData.w;

    // Hemisphere skylight, matching getAmbientLighting in Object.hlsl. Evaluated at the shading
    // normal, so the ground's blended normal maps steer it the same way they steer the sun --
    // worldNormal, the geometric one, stays the blend target and the fallback. AmbientScale
    // (TESR_TerrainData.w) scales the weather ambient above but not this: the sky is a second,
    // independent light source, so SkylightingScale is its only strength knob and it survives
    // AmbientScale = 0.
    float3 skyDiffuseNormal = BlendShadingNormal(worldNormal, worldShadingNormal, SKY_AMBIENT_NORMAL);
    ambientColor += SkyAmbientRadiance(skyDiffuseNormal, TESR_TerrainSkyData.y) * SKY_AMBIENT_STRENGTH;
    float3 color = albedo;
    color = lerp(luma(albedo), color, TESR_TerrainExtraData.y);

    [branch]
    if (TESR_TerrainExtraData.x) {
        // PBR.
        float roughness = saturate((1 - gloss) * TESR_TerrainData.y);
        float metal = saturate(metallicness * TESR_TerrainData.x);
        float3 lighting = PBRSun(metal, roughness, color, normal, eyeDir, lightDir, lightColor);

        // What the ground reflects of the sky, the specular half of the same light source whose
        // diffuse half is in ambientColor above. Only this branch gets it: the vanilla branch
        // below renders no specular lobe at all, so there is nothing for a reflection to belong
        // to. The full shading normal, not the blended one -- see SKY_AMBIENT_NORMAL. worldView
        // points at the camera.
        float3 f0 = lerp(float(0.04).rrr, color, metal);
        float3 reflection = SkyAmbientSpecular(worldShadingNormal, worldView, roughness, f0,
                                               TESR_TerrainSkyData.y) * SKY_AMBIENT_STRENGTH;

        return max(0, lighting + ambientColor * color + reflection);
    } else {
        // Vanilla, no specular.
        float3 lighting = getVanillaLightingAtt(lightDir, 1.0, sunColor, eyeDir, normal, albedo, gloss, glossPower);
        
        return lighting + ambientColor * color;
    }
}
