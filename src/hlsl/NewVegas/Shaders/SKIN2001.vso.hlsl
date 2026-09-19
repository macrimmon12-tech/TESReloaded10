// Skin vertex shader -- port of vanilla SKIN2001.vso (BONE-SKINNED skin geometry), plus the
// camera-relative world position the forward shadow lookup needs.
//
// This is the vertex shader the game actually pairs with SKIN2000.pso for FaceGenFace
// (traced: "Pass 263 BSSM_ADT_SFg, FaceGenFace (SKIN2001.vso SKIN2000.pso)"), so it is the one
// that matters for NPC faces. Removing this file falls back to vanilla, and the pixel shader's
// sentinel declines to shadow rather than reading garbage.
//
// Skinning goes through Includes/SkinHelpers.hlsl rather than being hand-rolled. Those helpers
// are what ObjectTemplate's SKIN variants use, so they are known to compile under the D3DX9
// HLSL compiler that NVR actually uses -- which is older than the SDK fxc and rejects some
// constructs fxc accepts. The index/weight pairing is identical to vanilla: offset comes from
// blendIndices.zyxw pre-scaled by 3 (765.01 = 255 * 3, each bone being three float4 rows), and
// offset.x pairs with blendWeight.x.
//
// REGISTER NOTE
// Bones[54] occupies c44..c97 and is indexed RELATIVELY. Shadow.hlsl's default pins at
// c100/c104 sit just past it -- which is exactly why those defaults were chosen. Do not add
// constants below c100 here.
#include "includes/Shadow.hlsl"
#include "includes/Helpers.hlsl"
#include "includes/SkinHelpers.hlsl"

row_major float4x4 SkinModelViewProj : register(c1);
float4 FogParam      : register(c14);
float3 FogColor      : register(c15);
float4 EyePosition   : register(c16);
float4 LightData[10] : register(c25);
float4 Bones[54]     : register(c44);

struct VS_INPUT {
    float4 position     : POSITION;
    float3 tangent      : TANGENT;
    float3 binormal     : BINORMAL;
    float3 normal       : NORMAL;
    float2 uv           : TEXCOORD0;
    float4 color        : COLOR0;
    float3 blendWeight  : BLENDWEIGHT;
    float4 blendIndices : BLENDINDICES;
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

    float4 offset = IN.blendIndices.zyxw * 765.01001;
    float4 blend = IN.blendWeight.xyzz;
    blend.w = 1 - weight(IN.blendWeight.xyz);

    // Each basis vector is skinned then normalised, as vanilla does.
    float3x3 tbn = BonesTransformTBN(Bones, offset, blend, IN.tangent, IN.binormal, IN.normal);

    float4 position = float4(IN.position.xyz, 1.0f);
    position.xyz = BonesTransformPosition(Bones, offset, blend, position);
    position.w = 1;

    OUT.position = mul(SkinModelViewProj, position);

    // Sun direction into the skinned tangent basis, then normalised.
    OUT.lightDir = float4(normalize(mul(tbn, LightData[0].xyz)), 1.0f);

    // Eye direction, normalised in object space then again in tangent space, as vanilla does.
    float3 eye = normalize(EyePosition.xyz - position.xyz);
    OUT.eyeDir = normalize(mul(tbn, eye));

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
