// Skin vertex shader -- port of vanilla SKIN2000.vso (unskinned skin geometry), plus the
// camera-relative world position the forward shadow lookup needs.
//
// Translated instruction-for-instruction from the vanilla vs_2_0 disassembly. The only
// addition is shadowWorldPos; everything else reproduces vanilla exactly. Removing this file
// falls back to the vanilla vertex shader, and the pixel shader's sentinel declines to shadow.
//
// Pairs with SKIN2000.pso, which reads TEXCOORD0/1/6 -- TEXCOORD4 is free.
//
// Registers used by vanilla: ModelViewProj c0-c3, FogParam c14, FogColor c15, EyePosition c16,
// LightData c25. No relative addressing, so Shadow.hlsl's default c100/c104 pins are clear.
#include "includes/Shadow.hlsl"

row_major float4x4 ModelViewProj : register(c0);
float4 FogParam     : register(c14);
float3 FogColor     : register(c15);
float4 EyePosition  : register(c16);
float4 LightData[10] : register(c25);

struct VS_INPUT {
    float4 position : POSITION;
    float3 tangent  : TANGENT;
    float3 binormal : BINORMAL;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VS_OUTPUT {
    float4 position       : POSITION;
    float2 uv             : TEXCOORD0;
    float4 lightDir       : TEXCOORD1;   // xyz: sun direction in tangent space, w: 1
    float4 shadowWorldPos : TEXCOORD4;   // the only addition to vanilla
    float3 eyeDir         : TEXCOORD6;   // eye direction in tangent space
    float4 color          : COLOR0;
    float4 fog            : COLOR1;      // rgb: fog colour, w: fog amount
};

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    // Sun direction into tangent space, then normalised.
    float3 l = float3(dot(IN.tangent,  LightData[0].xyz),
                      dot(IN.binormal, LightData[0].xyz),
                      dot(IN.normal,   LightData[0].xyz));
    OUT.lightDir = float4(normalize(l), 1.0f);

    // Eye direction: normalised in object space first, then taken into tangent space and
    // normalised again -- vanilla does both normalisations.
    float3 e = normalize(EyePosition.xyz - IN.position.xyz);
    OUT.eyeDir = normalize(float3(dot(IN.tangent, e), dot(IN.binormal, e), dot(IN.normal, e)));

    OUT.position = mul(ModelViewProj, IN.position);

    // Fog, from the clip-space xyz distance.
    float fogDist = length(OUT.position.xyz);
    float fogStrength = saturate((FogParam.x - fogDist) / FogParam.y);
    OUT.fog.rgb = FogColor;
    OUT.fog.w = pow(1.0f - fogStrength, FogParam.z);

    OUT.uv = IN.uv;
    OUT.color = IN.color;

    OUT.shadowWorldPos = float4(GetShadowWorldPos(OUT.position), SHADOW_VS_SENTINEL);

    return OUT;
};
