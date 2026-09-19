// SpeedTree leaf VS -- BSSM_LEAVES_AD. Transcription of vanilla STLEAF001.vso.
// vs_2_0 -> vs_3_0 to pair with the ps_3_0 shadow lookup.
//
// Engine uploads stop at c81 (vanilla defs sit at c82+), so Shadow.hlsl's c100/c104 are clear.
// Added: TEXCOORD3 sun-only, TEXCOORD4 shadowWorldPos. TEXCOORD1 stays vanilla's combined
// lighting, so the PS matches vanilla at shadow = 1.
#include "includes/Shadow.hlsl"

row_major float4x4 ModelViewProj : register(c0);
float4 AmbientColor  : register(c5);
float4 DiffuseColor  : register(c6);
float4 FogParam      : register(c8);   // x: start, y: range, z: power
float4 FogColor      : register(c9);
float4 SunScale      : register(c10);  // .x
float4 SunDir        : register(c11);
float4 CameraRight   : register(c13);
float4 CameraUp      : register(c14);
float4 WindA         : register(c15);
float4 WindB         : register(c16);
float4 NormalAdjust  : register(c17);  // .y

float4 BranchRows[16] : register(c18);  // 4 matrices, row stride 1, matrix stride a0.y
float4 LeafData[48]   : register(c34);

struct VS_INPUT {
    float4 position     : POSITION;
    float3 normal       : NORMAL;
    float2 uv           : TEXCOORD0;
    float4 blendIndices : BLENDINDICES;   // x: branch blend, y: matrix idx, z: leaf idx + scale, w: corner
};

struct VS_OUTPUT {
    float4 position       : POSITION;
    float2 uv             : TEXCOORD0;
    float4 lighting       : TEXCOORD1;   // vanilla combined
    float4 fog            : TEXCOORD2;   // rgb colour, w amount
    float3 sun            : TEXCOORD3;   // shadowable part of .lighting
    float4 shadowWorldPos : TEXCOORD4;
};

// Wrap to -pi..pi (mad/frc/mad).
float WrapPi(float v, float scale) {
    return frac(v * scale + 0.5f) * 6.28318548f - 3.14159274f;
}

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float4 v0 = IN.position;
    float4 v3 = IN.blendIndices;


    float a = WrapPi(v3.z * 0.020833334f + WindB.y, 0.499999583f);
    a = WrapPi(sin(a) * WindB.z * WindB.x, 0.159154937f);
    // xy rotation of the camera basis. Identity at zero wind: offset = p*Right + q*Up.
    float3 axis1 = float3(cos(a), -sin(a), 0.0f);
    float3 tan1  = float3(sin(a),  cos(a), 0.0f);


    float b = WrapPi(v3.z * 0.020833334f + WindA.y, 0.499999583f);
    b = WrapPi(sin(b) * WindA.z * WindA.x, 0.159154937f);
    float3 axis2 = float3(0.0f, cos(b), -sin(b));
    float3 tan2  = float3(0.0f, sin(b), cos(b));

    int a0x = (int)(v3.z - frac(v3.z));
    int a0y = (int)(v3.y - frac(v3.y));

    float4 leaf = v3.w * LeafData[a0x];

    float d1 = dot(axis1, CameraRight.xyz);
    float d2 = dot(axis1, CameraUp.xyz);

    float p = dot(axis2, leaf.xyz);
    float4 r5;
    r5.x = d1 * p;
    r5.y = p * dot(tan1, CameraRight.xyz);
    r5.zw = p * CameraRight.zw;

    float q = dot(tan2, leaf.xyz);
    float4 r1;
    r1.x = d2 * q;
    r1.y = dot(tan1, CameraUp.xyz) * q;
    r1.zw = q * CameraUp.zw;

    float4 local = r5 + r1 + v0;

    float4 skinned;
    skinned.x = dot(BranchRows[a0y + 0], local);
    skinned.y = dot(BranchRows[a0y + 1], local);
    skinned.z = dot(BranchRows[a0y + 2], local);
    skinned.w = dot(BranchRows[a0y + 3], local);

    float4 pos = lerp(local, skinned, v3.x);

    // Nudged along the leaf dir. 4-component normalise (dp4 r4, r4).
    float3 leafDir = leaf.xyz * rsqrt(dot(leaf, leaf));
    float3 N = normalize(leafDir * NormalAdjust.y + IN.normal);

    float NdotL = saturate(dot(N, SunDir.xyz));
    float lightScale = frac(v3.z);

    float3 sun = lightScale * (NdotL * DiffuseColor.rgb * SunScale.x);
    OUT.sun = sun;
    OUT.lighting = float4(sun + lightScale * AmbientColor.rgb, 1.0f);

    OUT.position = mul(ModelViewProj, pos);

    // Fog distance is (x, y, w-z), not xyz.
    float3 fogVec = float3(OUT.position.x, OUT.position.y, OUT.position.w - OUT.position.z);
    float fogDist = length(fogVec);
    float t = saturate((FogParam.x - fogDist) / FogParam.y);
    OUT.fog.rgb = FogColor.rgb;
    OUT.fog.w = pow(1.0f - t, FogParam.z);

    OUT.uv = IN.uv;
    OUT.shadowWorldPos = float4(GetShadowWorldPos(OUT.position), SHADOW_VS_SENTINEL);

    return OUT;
};
