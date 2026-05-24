// Per-channel wrapped diffuse — simulates skin's differential spectral SSS.
// R wraps widest (deep red backscatter), B wraps least (tight blue response).
// Wrap depth is controlled by TESR_SkinData.z (MaterialThickness).
float3 WrapDiffuse(float3 lightDirection, float3 normal) {
    float NdotL = dot(normal, lightDirection);
    float wrapR = TESR_SkinData.z * 1.5;
    float wrapG = TESR_SkinData.z * 0.75;
    float r = saturate((NdotL + wrapR) / (1.0 + wrapR));
    float g = saturate((NdotL + wrapG) / (1.0 + wrapG));
    float b = saturate(NdotL);
    return float3(r, g, b);
}

// Normalized Blinn-Phong for skin highlights.
// TESR_SkinData.y [0,1] maps to shininess [1,128]; TESR_SkinData.x scales intensity.
float SkinSpecular(float3 lightDirection, float3 eyeDirection, float3 normal) {
    float3 H = normalize(lightDirection + eyeDirection);
    float NdotH = saturate(dot(normal, H));
    float NdotL = saturate(dot(normal, lightDirection));
    return pow(NdotH, max(TESR_SkinData.y * 128.0, 1.0)) * NdotL * TESR_SkinData.x;
}

// Back-scatter transmittance: light passing through thin skin geometry,
// tinted by TESR_SkinColor. TESR_SkinData.w (RimScalar) controls strength.
float3 SkinTransmittance(float3 lightDirection, float3 eyeDirection, float3 normal, float3 lightColor) {
    float backNL = saturate(dot(-normal, lightDirection) + TESR_SkinData.z);
    float viewScatter = saturate(dot(-eyeDirection, lightDirection));
    return backNL * viewScatter * TESR_SkinData.w * lightColor * TESR_SkinColor.rgb;
}

float3 getNormal(float2 uv) {
    return normalize(expand(tex2D(NormalMap, uv).xyz));
}

float3 ApplyVertexColor(float3 baseColor, float3 vertexColor, float4 toggles) {
    return toggles.x <= 0.0 ? baseColor : (baseColor * vertexColor);
}

float3 ApplyFog(float3 baseColor, float4 fogColor, float4 toggles) {
    return toggles.y <= 0.0 ? baseColor : ((fogColor.a * (fogColor.rgb - baseColor)) + baseColor);
}

// Blends the base texture with FaceGen customization maps (vanilla formula).
float4 getBaseColor(float2 uv, sampler2D FaceGenMap0Buffer, sampler2D FaceGenMap1Buffer, sampler2D BaseColorBuffer) {
    float3 faceGenMap0 = tex2D(FaceGenMap0Buffer, uv).rgb;
    float3 faceGenMap1 = tex2D(FaceGenMap1Buffer, uv).rgb;
    float4 baseTexture = tex2D(BaseColorBuffer, uv);
    // Vanilla formula (2 * ((expand(m0) + base) * (2 * m1))) amplifies the detail map by up to 4x,
    // producing severe splotching. FaceGenMap1 neutral is ~0.25 (4 * 0.25 = 1.0 = identity on base).
    // FaceGenMap0 detail contribution is scaled down to keep it a subtle overlay.
    float3 blended = saturate((baseTexture.rgb + expand(faceGenMap0) * 0.25) * (4.0 * faceGenMap1));
    return float4(blended, baseTexture.a);
}

// Point light contribution with SSS tint from TESR_SkinColor.
float3 getPointLight(float3 LightDirection, float3 eyeDirection, float3 LightColor, float3 glowTexture, float3 normal, float Attenuation1, float Attenuation2) {
    float3 SSScolor = lerp(LightColor, TESR_SkinColor.rgb, 0.5);
    float fresnel = sqr(1.0 - shades(normal, eyeDirection));
    float diffuse = dot(normal, LightDirection);
    float diffuse2 = saturate((diffuse + 0.3) * 0.769230783);
    diffuse = saturate(diffuse);
    float3 contribution = saturate(((3.0 - diffuse2 * 2.0) * sqr(diffuse2)) - ((3.0 - diffuse * 2.0) * sqr(diffuse))) * TESR_SkinColor.rgb;
    contribution += diffuse * LightColor;
    contribution += fresnel * shades(eyeDirection, -LightDirection) * SSScolor;
    return saturate((1.0 - Attenuation1) - Attenuation2) * contribution;
}
