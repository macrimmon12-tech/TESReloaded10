// Skin Shader for when only the sun light is used
//
// Parameters:

float4 AmbientColor : register(c1);
sampler2D BaseMap : register(s0);
sampler2D FaceGenMap0 : register(s2);
sampler2D FaceGenMap1 : register(s3);
sampler2D NormalMap : register(s1);
float4 PSLightColor[10] : register(c3);
float4 Toggles : register(c27); // x:bUseVertexColors, y:fUnknown_FogRelated, z:fSpecularity, w:fAlphaTestRef

float4 TESR_ReciprocalResolution;
float4 TESR_SkinData;
float4 TESR_SkinColor;
float4 TESR_DebugVar;


#include "Includes/Helpers.hlsl"

// Registers:
//
//   Name         Reg   Size
//   ------------ ----- ----
//   AmbientColor const_1       1
//   PSLightColor[0] const_3       1
//   Toggles      const_27      1
//   BaseMap      texture_0       1
//   NormalMap    texture_1       1
//   FaceGenMap0  texture_2       1
//   FaceGenMap1  texture_3       1
//


// Structures:

#include "Includes/PBRScale.hlsl"

struct VS_INPUT {
    float2 BaseUV : TEXCOORD0;
    float4 shadowWorldPos : TEXCOORD4;   // from ObjectTemplate VS			            // UV
    float3 texcoord_1 : TEXCOORD1;			// light data in tangent space
    float3 texcoord_6 : TEXCOORD6;			// eye data in tangent space
    float3 position : SV_POSITION;			// light data in tangent space
    float3 color_0 : COLOR0;                // vertex color?
    float4 color_1 : COLOR1;                // fog contribution?
};

struct VS_OUTPUT {
    float4 color_0 : COLOR0;
};

#include "Includes/Shadow.hlsl"
#include "Includes/Skin.hlsl"


VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;


    float3 lightDirection = normalize(IN.texcoord_1);
    float3 eyeDirection = normalize(IN.texcoord_6);
    float3 normal = getNormal(IN.BaseUV);


    float3 lighting = GetLighting(lightDirection, eyeDirection, normal, PBRLight(PSLightColor[0]).rgb);
    float spec = GetSpecular(lightDirection, eyeDirection, normal, PBRLight(PSLightColor[0]).rgb);


#if FORWARD_SHADOWS
    // Forward sun shadow. Scales the SUN terms only; ambient-driven terms are untouched.
    // ddx/ddy must stay at top level, outside dynamic flow control.
    float3 shadowNormal = GetShadowGeometricNormal(IN.shadowWorldPos.xyz);
    float sunShadow = SHADOW_VS_PRESENT(IN.shadowWorldPos.w)
                    ? GetSunShadow(IN.shadowWorldPos.xyz, shadowNormal)
                    : 1.0f;
    lighting *= sunShadow;
    spec     *= sunShadow;
#endif

    float4 baseColor = getBaseColor(IN.BaseUV, FaceGenMap0, FaceGenMap1, BaseMap);
    baseColor.rgb = ApplyVertexColor(baseColor.rgb, IN.color_0.rgb, Toggles);

    float4 color = AmbientColor.a >= 1 ? 0 : (baseColor.a - Toggles.w);
    // Vanilla: max(0, sun*NdotL + sun*0.5*sat(dot(E,-L))*(1-NdotV)^2 + Ambient) * albedo
    // The middle term is backscatter; GetLighting's fresnel covers it.
    // Specular sits outside the albedo multiply. SKIN_SPECULAR_STRENGTH defaults to 0.
    float3 finalColor = max(lighting + PBRAmbient(AmbientColor.rgb) + SkyAmbient(shadowNormal, SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f), 0) * baseColor.rgb + spec;

    color.rgb = ApplyFog(finalColor, IN.color_1, Toggles);
    color.a = baseColor.a * AmbientColor.a;

    OUT.color_0 = color;
    return OUT;
};

// approximately 42 instruction slots used (4 texture, 38 arithmetic)
