

// Requires the following registers:
//
//   Name            Reg   Size
//   --------------- ----- ----
//   EyePos          const_1       1
//   shallowColor    const_2       1
//   deepColor       const_3       1
//   ReflectionColor const_4       1
//   FresnelRI       const_5       1  //x: reflectamount, y:fresnel, w: opacity, z:speed
//   BlendRadius     const_6       1
//   VarAmounts      const_8       1  // x: water glossiness y: reflectivity z: refrac, w: lod
//   FogParam        const_9       1
//   FogColor        const_10      1
//   DepthFalloff    const_11      1  // start / end depth fog
//   SunDir          const_12      1
//   SunColor        const_13      1
//   ReflectionMap   texture_0       1
//   RefractionMap   texture_1       1
//   NoiseMap        texture_2       1
//   DisplacementMap texture_3       1
//   DepthMap        texture_4       1
//   float4 TESR_WaveParams : register(c14); // x: choppiness, y:wave width, z: wave speed, w: reflectivity?
//   float4 TESR_WaterVolume : register(c15); // x: caustic strength, y:shoreFactor, w: turbidity, z: caustic strength S ?
//   float4 TESR_WaterSettings : register(c16); // x: caustic strength, y:depthDarkness, w: turbidity, z: caustic strength S ?
//   float4 TESR_GameTime : register(c17);
//   float4 TESR_SkyColor : register(c18);
//   float4 TESR_SunDirection : register(c19);
//   sampler2D TESR_samplerWater : register(s5);


struct PS_INPUT {
    float4 LTEXCOORD_0 : TEXCOORD0_centroid;     // world position of underwater points
    float4 LTEXCOORD_1 : TEXCOORD1_centroid;     // local position on plane object surface
    float4 LTEXCOORD_2 : TEXCOORD2_centroid;     // modelviewproj matrix 1st row 
    float4 LTEXCOORD_3 : TEXCOORD3_centroid;     // modelviewproj matrix 2nd row 
    float4 LTEXCOORD_4 : TEXCOORD4_centroid;     // modelviewproj matrix 3rd row 
    float4 LTEXCOORD_5 : TEXCOORD5_centroid;     // modelviewproj matrix 4th row 
    float4 LTEXCOORD_6 : TEXCOORD6;              // displacement sampling position
    float2 LTEXCOORD_7 : TEXCOORD7;              // waves sampling position
    float4 WorldPosition : TEXCOORD8;
};

struct PS_OUTPUT {
    float4 color_0 : COLOR0;
};

#include "Includes/PBR.hlsl"

float4 getScreenpos(PS_INPUT IN){
    float4 screenPos;  // point coordinates in screen space for water surface
    screenPos.x = dot(IN.LTEXCOORD_2, IN.LTEXCOORD_1);
    screenPos.w = dot(IN.LTEXCOORD_5, IN.LTEXCOORD_1);
    screenPos.y = screenPos.w - dot(IN.LTEXCOORD_3, IN.LTEXCOORD_1);
    screenPos.z = dot(IN.LTEXCOORD_4, IN.LTEXCOORD_1);
    
    return screenPos;
}

// Accumulate multiple waves then reconstruct: normalize(float3(n.xy, 1.0 + n.z))
float3 GerstnerNormal(float2 pos, float2 dir, float wavelength, float kA, float steepness, float phase) {
    dir = normalize(dir);
    float k = 2.0 * PI / wavelength;
    float phi = k * dot(dir, pos) + phase;
    return float3(-dir.x * kA * cos(phi), -dir.y * kA * cos(phi), -steepness * kA * sin(phi));
}

float3 getWaveTexture(PS_INPUT IN, float distance, float4 waveParams) {
    float2 texPos = IN.LTEXCOORD_7;

    float waveWidth = waveParams.y;
    float choppiness = waveParams.x;
    float speed = TESR_GameTime.x * 0.002 * waveParams.z;
    float steepness = saturate(choppiness) * 0.4;
    float baseWavelength = 0.5 / max(waveWidth, 0.01);

    // Four Gerstner waves: tiling-free primary structure replacing the two large-scale texture samples
    float kA = 0.4;
    float3 n = float3(0, 0, 0);
    n += GerstnerNormal(texPos, float2( 1.0,  2.0), baseWavelength,        kA,        steepness, speed * 1.00);
    n += GerstnerNormal(texPos, float2(-2.0,  3.0), baseWavelength * 0.71, kA * 0.85, steepness, speed * 1.25);
    n += GerstnerNormal(texPos, float2( 3.0, -2.0), baseWavelength * 0.53, kA * 0.65, steepness, speed * 1.55);
    n += GerstnerNormal(texPos, float2(-1.0, -3.0), baseWavelength * 0.37, kA * 0.45, steepness, speed * 1.90);

    float3 primaryNormal = normalize(float3(n.xy, 1.0 + n.z));

    // Single micro-detail texture sample for high-frequency surface ripples
    float3 microDetail = expand(tex2D(TESR_samplerWater, texPos * 4.0 * waveWidth + normalize(float2(1, 3)) * speed)).xyz * 0.5;

    float3 waveTexture = float3(primaryNormal.xy + microDetail.xy, primaryNormal.z);
    waveTexture.z *= 1.0 / max(choppiness, 0.000001);
    waveTexture = normalize(waveTexture);

    return waveTexture;
}

float4 getReflectionSamplePosition(PS_INPUT IN, float3 surfaceNormal, float refractionCoeff) {
    int4 const_7 = {0, 2, -1, 1}; // used to cancel/double/invert vector components

    float4 samplePosition;
    samplePosition.xy = ((refractionCoeff * surfaceNormal.xy)) + IN.LTEXCOORD_1.xy;
    // waveTexture.xy = ((refractionCoeff * surfaceNormal.xy) / IN.LTEXCOORD_0.w) + IN.LTEXCOORD_1.xy;
    samplePosition.zw = (IN.LTEXCOORD_1.z * const_7.wx) + const_7.xw;

    float4 reflectionPos = mul(float4x4(IN.LTEXCOORD_2, IN.LTEXCOORD_3, IN.LTEXCOORD_4, IN.LTEXCOORD_5), samplePosition); // convert local normal to view space

    return reflectionPos;
}

float3 getDisplacement(PS_INPUT IN, float blendRadius, float3 surfaceNormal){
    // sample displacement and mix with the wave texture
    float4 displacement = tex2D(DisplacementMap, IN.LTEXCOORD_6.xy);

    displacement.xy = (displacement.zw - 0.5) * blendRadius / 2;

    // sample displacement and mix with the wave texture
    float3 DisplacementNormal = normalize(reconstructZ(displacement.xy));

    surfaceNormal = float3(surfaceNormal.xy + DisplacementNormal.xy * 2,  surfaceNormal.z * DisplacementNormal.z);
    surfaceNormal = normalize(surfaceNormal);
    return surfaceNormal;
}

float4 getLightTravel(float3 refractedDepth, float4 shallowColor, float4 deepColor, float sunLuma, float4 waterSettings, float4 color){
    float4 waterColor = lerp(shallowColor, deepColor, refractedDepth.y); 
    //float4 waterColor = shallowColor; 
    float depthDarknessPower = saturate(pows((1 - waterSettings.y), 3)); // high darkness means low values
    float3 result = color.rgb * lerp(0.7, lerp(waterColor.rgb * depthDarknessPower, 1, depthDarknessPower) , refractedDepth.x) ; //never reach 1 so that water is always absorbing some light
    return float4(result, 1);
}

float4 getTurbidityFog(float3 refractedDepth, float4 shallowColor, float4 waterVolume, float sunLuma, float4 color){
    float turbidity = waterVolume.z;

    float depth = pows(refractedDepth.x, turbidity);

    float fogCoeff = 1 - saturate((FogParam.z - (refractedDepth.x * FogParam.z)) / FogParam.w);
    float3 fog = shallowColor.rgb * sunLuma;

    float3 result = lerp(color.rgb, fog.rgb, saturate(fogCoeff * FogColor.a * turbidity));

    // return float4(1 - refractedDepth.yyy, 1);
    return float4(result, 1);
}

float4 getDiffuse(float3 surfaceNormal, float3 lightDir, float3 eyeDirection, float distance, float4 diffuseColor, float4 color){
    float verticalityFade =  (1 - shades(eyeDirection, float3(0, 0, 1)));
    float distanceFade = smoothstep(0, 1, distance * 0.001);
    float diffuse = shades(lightDir, surfaceNormal) * verticalityFade * distanceFade; // increase intensity with distance
    float3 result = lerp(color.rgb, diffuseColor.rgb, saturate(diffuse));

    return float4(result, 1);
}

float4 getFresnel(float3 surfaceNormal, float3 eyeDirection, float4 reflection, float reflectivity, float4 color){
    // float4 getReflections(float3 surfaceNormal, eyeDirection, float4 reflection, float4 color){
    float fresnelCoeff = saturate(pow(1 - dot(eyeDirection, surfaceNormal), 5));
    float reflectionLuma = luma(reflection);
    float lumaDiff = saturate(reflectionLuma - luma(color));

    //float4 reflectionColor = lerp (reflectionLuma * linearize(ReflectionColor), reflection, reflectionLuma * VarAmounts.y) * 0.7;
    float4 reflectionColor = lerp (reflectionLuma * linearize(ReflectionColor), reflection, reflectionLuma) * 0.7;
	float3 result = lerp(color.rgb, reflection.rgb , saturate((fresnelCoeff * 0.8 + 0.2 * lumaDiff) * reflectivity));

    return float4(result, 1);
}

float4 getSpecular(float3 surfaceNormal, float3 lightDir, float3 eyeDirection, float3 specColor, float roughness, float4 color){
    float specularBoost = 6;
    float3 normal = normalize(surfaceNormal);

    // Find the closest point on the sun disk to the specular reflection ray.
    // This accounts for the sun's angular size rather than treating it as a point,
    // giving a broader and more physically natural highlight on water.
    float3 R = reflect(-eyeDirection, normal);
    float radius = sin(SUN_RADIUS);
    float dist = cos(SUN_RADIUS);
    float RdotL = dot(R, lightDir);
    float3 tangent = R - RdotL * lightDir;
    float tangentLen = length(tangent);
    float3 sunDir = (RdotL < dist && tangentLen > 0.001) ?
        normalize(dist * lightDir + (tangent / tangentLen) * radius) : R;

    float3 halfway = normalize(eyeDirection + sunDir);
    float NdotH = shades(normal, halfway);
    float NdotV = max(shades(normal, eyeDirection), 0.00001);
    float NdotS = max(shades(normal, sunDir), 0.00001);

    float3 Ks = FresnelShlick(0.04, halfway, eyeDirection);
    float3 result = color.rgb + BRDF(roughness, Ks, NdotV, NdotS, NdotH) * specColor * specularBoost * NdotS;
    return float4(result, color.a);
}

float4 getPointLightSpecular(float3 surfaceNormal, float4 lightPosition, float3 worldPosition, float3 eyeDirection, float3 specColor, float4 color){
    if (lightPosition.w == 0) return color;

    float specularBoost = 1;
    float glossiness = 20;

    float3 lightDir = lightPosition.xyz - worldPosition;
    float distance = length(lightDir) / lightPosition.w;

        // radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
    float s = saturate(distance * distance); 
    float atten = saturate(((1 - s) * (1 - s)) / (1 + 5.0 * s));

    //return color + getSpecular(surfaceNormal, normalize(lightDir), eyeDirection, specColor * atten, color);
    lightDir = normalize(lightDir);
    float3 H = normalize(lightDir + eyeDirection);
    float NdotL = shades(surfaceNormal, lightDir);
    float NdotV = shades(surfaceNormal, eyeDirection);
    float NdotH = shades(surfaceNormal, H);

    float3 Ks = FresnelShlick(0.08, H, eyeDirection);
    color.rgb += BRDF(0.02, Ks, NdotV, NdotL, NdotH) * specColor * atten * NdotL;
    // color.rgb += pows(shades(H, surfaceNormal), glossiness) * linearize(float4(specColor, 1)).rgb * specularBoost * atten;

    // color.rgb += pows(shades(H, surfaceNormal), 100) * specColor * 10 * atten;
    return color;
}


float4 getShoreFade(PS_INPUT IN, float depth, float shoreSpeed, float shoreFactor, float4 color){
    float scale = 0.07;
    shoreSpeed *= 0.1;
    shoreFactor *= 0.1;

    float shoreAnimation = sin(IN.LTEXCOORD_7.x/scale + TESR_GameTime.x * shoreSpeed);
    shoreAnimation *= cos(IN.LTEXCOORD_7.y/scale + TESR_GameTime.x * shoreSpeed);
    shoreAnimation = compress(shoreAnimation); // create a grid of gradient values from 0 to 1

    float depthGradient = smoothstep(saturate(shoreFactor) * compress(sin(TESR_GameTime.x * shoreSpeed) * shoreAnimation), 0, depth);

    color.a = 1 - depthGradient;
    return color;
}


float3 ComputeRipple(sampler2D puddlesSampler, float2 UV, float CurrentTime, float Weight)
{
    float4 Ripple = tex2D(puddlesSampler, UV);
    Ripple.yz = expand(Ripple.yz); // convert from 0/1 to -1/1 

    float period = frac(Ripple.w + CurrentTime);
    float TimeFrac = period - 1.0f + Ripple.x;
    float DropFactor = saturate(0.2f + Weight * 0.8f - period);
    float FinalFactor = DropFactor * Ripple.x * sin( clamp(TimeFrac * 9.0f, 0.0f, 3.0f) * PI);

    return float3(Ripple.yz * FinalFactor * 0.35f, 1.0f);
}


float3 getRipples(PS_INPUT IN, sampler2D puddlesSampler, float3 surfaceNormal, float distance, float rainCoeff){
    float distanceFade = 1 - saturate(invlerp(0, 3500, distance));

    if (!rainCoeff || !distanceFade) return surfaceNormal;

    // sample and combine rain ripples
    float4 time = float4(0.96f, 0.97f,  0.98f, 0.99f) * 0.07; // Ripple timing

	float2 rippleUV = IN.LTEXCOORD_7 * 5; // scale coordinates
	float4 Weights = float4(1, 0.75, 0.5, 0.25) * rainCoeff;
	Weights = saturate(Weights * 4) * 2 * distanceFade;
	float3 Ripple1 = ComputeRipple(puddlesSampler, rippleUV + float2( 0.25f,0.0f), time.x * TESR_GameTime.x, Weights.x);
	float3 Ripple2 = ComputeRipple(puddlesSampler, rippleUV * 1.1 + float2(-0.55f,0.3f), time.y * TESR_GameTime.x, Weights.y);
	float3 Ripple3 = ComputeRipple(puddlesSampler, rippleUV * 1.3 + float2(0.6f, 0.85f), time.z * TESR_GameTime.x, Weights.z);
	float3 Ripple4 = ComputeRipple(puddlesSampler, rippleUV * 1.5 + float2(0.5f,-0.75f), time.w * TESR_GameTime.x, Weights.w);

	float4 Z = lerp(1, float4(Ripple1.z, Ripple2.z, Ripple3.z, Ripple4.z), Weights);
	float3 ripple = float3( Weights.x * Ripple1.xy + Weights.y * Ripple2.xy + Weights.z * Ripple3.xy + Weights.w * Ripple4.xy, Z.x * Z.y * Z.z * Z.w);
	float3 ripnormal = normalize(ripple);
    
    float3 combnom = normalize(float3(ripnormal.xy + surfaceNormal.xy, surfaceNormal.z));

    return combnom;
}

// Returns raw caustic intensity sampled from a caustic texture at world XY position.
// Apply as: color.rgb += color.rgb * pows(result, 2.0) * strength * 100;
// Two scrolling layers combined with min() produce the sharp bright-line caustic pattern.
float getCausticsFromAbove(sampler2D causticsSampler, float2 worldXY, float gameTime, float waveSpeed) {
    float speed = gameTime * 0.01 * waveSpeed;
    float scale = 0.002;
    float layer1 = tex2D(causticsSampler, worldXY * scale        + speed * normalize(float2(-1.2, -2.5))).r;
    float layer2 = tex2D(causticsSampler, worldXY * scale * 1.2  + speed * normalize(float2( 0.5,  2.0))).r;
    return pows(min(layer1, layer2), 1.3) * 4.0;
}