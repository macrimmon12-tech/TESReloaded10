// Skin with one point light
//
// Parameters:

float4 AmbientColor : register(c1);
sampler2D AttenuationMap : register(s5);
sampler2D BaseMap : register(s0);
sampler2D FaceGenMap0 : register(s2);
sampler2D FaceGenMap1 : register(s3);
sampler2D GlowMap : register(s4);
sampler2D NormalMap : register(s1);
float4 PSLightColor[10];
float4 Toggles : register(c27);

float4 TESR_SkinData;
float4 TESR_SkinColor;


// Registers:
//
//   Name           Reg   Size
//   -------------- ----- ----
//   AmbientColor   const_1       1
//   PSLightColor[0]   const_3       2
//   Toggles        const_27      1
//   BaseMap        texture_0       1
//   NormalMap      texture_1       1
//   FaceGenMap0    texture_2       1
//   FaceGenMap1    texture_3       1
//   GlowMap        texture_4       1
//   AttenuationMap texture_5       1
//

#include "Includes/Helpers.hlsl"

struct VS_INPUT {
    float2 BaseUV : TEXCOORD0;                      // uv
    float3 color_0 : COLOR0;                        // vertex color
    float4 color_1 : COLOR1;                        // fog color
    float3 texcoord_1 : TEXCOORD1_centroid;         // sun light direction
    float3 texcoord_2 : TEXCOORD2_centroid;         // point light direction
    float4 texcoord_4 : TEXCOORD4;                  // attenuation map UVs
    float3 texcoord_6 : TEXCOORD6_centroid;         // eye direction
};

struct VS_OUTPUT {
    float4 color_0 : COLOR0;
};

#include "Includes/Skin.hlsl"

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float3 lightDirection = normalize(IN.texcoord_1);
    float3 eyeDirection = normalize(IN.texcoord_6);
    float3 normal = getNormal(IN.BaseUV);
    float3 glowTexture = tex2D(GlowMap, IN.BaseUV).rgb;

    float4 baseColor = getBaseColor(IN.BaseUV, FaceGenMap0, FaceGenMap1, BaseMap);
    baseColor.rgb = ApplyVertexColor(baseColor.rgb, IN.color_0, Toggles);

    clip(AmbientColor.a >= 1 ? 0 : (baseColor.a - Toggles.w));

    float3 pointLightDirection = normalize(IN.texcoord_2);
    float atten1 = tex2D(AttenuationMap, IN.texcoord_4.xy).x;
    float atten2 = tex2D(AttenuationMap, IN.texcoord_4.zw).x;
    float3 pointLightLighting = getPointLight(pointLightDirection, eyeDirection, PSLightColor[2].rgb, glowTexture, normal, atten1, atten2);

    float3 sunColor = PSLightColor[1].rgb;
    float3 diffuse = WrapDiffuse(lightDirection, normal) * sunColor * TESR_SkinData.x;
    float3 transmit = SkinTransmittance(lightDirection, eyeDirection, normal, sunColor);
    float spec = SkinSpecular(lightDirection, eyeDirection, normal);

    float3 lighting = diffuse + transmit + pointLightLighting + AmbientColor.rgb;
    float3 finalColor = lighting * baseColor.rgb + spec * sunColor;

    OUT.color_0.rgb = ApplyFog(finalColor, IN.color_1, Toggles);
    OUT.color_0.a = baseColor.a * AmbientColor.a;

    return OUT;
}
