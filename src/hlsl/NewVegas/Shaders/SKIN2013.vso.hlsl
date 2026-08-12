// Skin vertex shader -- port of vanilla SKIN2013.vso (BONE-SKINNED skin, sun + two point
// lights), plus the camera-relative world position the forward shadow lookup needs.
//
// Pairs with SKIN2006.pso (traced: "Pass 240 BSSM_AD3_SFg, FaceGenFace (SKIN2013.vso
// SKIN2006.pso) - Vertex: vanilla"). Vertex and pixel shaders are NOT paired by name; confirm
// any new pairing with a shader trace before porting.
//
// Vanilla writes TEXCOORD0/1/2/3/4/5/7, so TEXCOORD6 is free and carries shadowWorldPos.
//
// This variant has NO vertex colour and NO fog -- it declares neither the COLOR0 input nor
// FogParam/FogColor, and writes no oD0/oD1. Do not add them back from the other skin ports.
// Blend weights arrive in v5 and indices in v6, one slot lower than SKIN2005.vso.
//
// REGISTER NOTE
// Bones[54] occupies c44..c97 and is indexed RELATIVELY. Shadow.hlsl's default pins at
// c100/c104 sit just past it. Do not add constants below c100 here.
#include "includes/Shadow.hlsl"
#include "includes/Helpers.hlsl"
#include "includes/SkinHelpers.hlsl"

row_major float4x4 SkinModelViewProj : register(c1);
float4 EyePosition   : register(c16);
float4 LightData[10] : register(c25);
float4 Bones[54]     : register(c44);

struct VS_INPUT {
    float4 position     : POSITION;
    float3 tangent      : TANGENT;
    float3 binormal     : BINORMAL;
    float3 normal       : NORMAL;
    float2 uv           : TEXCOORD0;
    float3 blendWeight  : BLENDWEIGHT;
    float4 blendIndices : BLENDINDICES;
};

struct VS_OUTPUT {
    float4 position       : POSITION;
    float2 uv             : TEXCOORD0;
    float3 lightDir       : TEXCOORD1;   // sun direction in tangent space
    float3 light2Dir      : TEXCOORD2;   // point light 1 direction in tangent space
    float3 light3Dir      : TEXCOORD3;   // point light 2 direction in tangent space
    float4 atten1         : TEXCOORD4;   // point light 1 attenuation UVs, w: 0.5
    float4 atten2         : TEXCOORD5;   // point light 2 attenuation UVs, w: 0.5
    float4 shadowWorldPos : TEXCOORD6;   // the only addition to vanilla
    float3 eyeDir         : TEXCOORD7;   // eye direction in tangent space
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
    OUT.lightDir = normalize(mul(tbn, LightData[0].xyz));

    // Eye direction, normalised in object space then again in tangent space, as vanilla does.
    float3 eye = normalize(EyePosition.xyz - position.xyz);
    OUT.eyeDir = normalize(mul(tbn, eye));

    // Point lights. LightData[n].xyz is the position and .w the radius. Vanilla normalises in
    // object space only -- the tangent basis is already unit length, so there is no second
    // normalise -- and builds each attenuation UV from the UNNORMALISED vector scaled by
    // 1/radius and biased into 0..1.
    float3 lightVec1 = LightData[1].xyz - position.xyz;
    OUT.light2Dir = mul(tbn, normalize(lightVec1));
    OUT.atten1 = float4(lightVec1 / LightData[1].w * 0.5f + 0.5f, 0.5f);

    float3 lightVec2 = LightData[2].xyz - position.xyz;
    OUT.light3Dir = mul(tbn, normalize(lightVec2));
    OUT.atten2 = float4(lightVec2 / LightData[2].w * 0.5f + 0.5f, 0.5f);

    OUT.uv = IN.uv;

    OUT.shadowWorldPos = float4(GetShadowWorldPos(OUT.position), SHADOW_VS_SENTINEL);

    return OUT;
};
