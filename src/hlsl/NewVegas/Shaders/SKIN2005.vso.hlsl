// Skin vertex shader -- port of vanilla SKIN2005.vso (BONE-SKINNED skin, sun + one point
// light), plus the camera-relative world position the forward shadow lookup needs.
//
// This is the vertex shader the game pairs with SKIN2002.pso for lit NPC faces (traced:
// "Pass 286 BSSM_ADT2_SFg, FaceGenFace (SKIN2005.vso SKIN2002.pso) - Vertex: vanilla"). The
// pixel shader was already replaced, which is why PBR lighting applied there while shadows did
// not: nothing stamped shadowWorldPos, so the sentinel declined to shadow. Removing this file
// restores that state rather than reading garbage.
//
// Vertex and pixel shaders are NOT paired by name -- SKIN2005.vso feeds SKIN2002.pso. Confirm
// any new pairing with a shader trace before porting.
//
// Vanilla writes TEXCOORD0/1/2/4/6, so TEXCOORD3 is free and carries shadowWorldPos.
//
// REGISTER NOTE
// Bones[54] occupies c44..c97 and is indexed RELATIVELY. Shadow.hlsl's default pins at
// c100/c104 sit just past it. Do not add constants below c100 here.
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
    float4 light2Dir      : TEXCOORD2;   // xyz: point light direction in tangent space, w: 1
    float4 shadowWorldPos : TEXCOORD3;   // the only addition to vanilla
    float4 atten          : TEXCOORD4;   // attenuation map UVs, w: 0.5
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

    // Point light. LightData[1].xyz is its position and .w the radius. Vanilla normalises in
    // object space only -- the tangent basis is already unit length, so it does not normalise
    // again afterwards -- and builds the attenuation UVs from the UNNORMALISED vector scaled by
    // 1/radius and biased into 0..1.
    float3 lightVec = LightData[1].xyz - position.xyz;
    OUT.light2Dir = float4(mul(tbn, normalize(lightVec)), 1.0f);
    OUT.atten = float4(lightVec / LightData[1].w * 0.5f + 0.5f, 0.5f);

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
