// UNOFFICIAL lit particles (Shaders.Particles, off by default): vanilla NOLIGHT016.vso, particle systems
// (BSSM_NOLIGHTING_PSYS and its premultiplied-alpha twin). The game's vs_2_0 shader ported as is, so it can pair
// with the vs_3_0-only lit NOLIGHTTEXVC.pso, plus the camera-relative world position (and the sentinel) for the
// pixel shader's sun shadow and point lights. Registers as in the game's shader: the engine sets them by number.

#include "includes/Shadow.hlsl"
#define PARTICLE_VS
#include "includes/ParticleLight.hlsl"

row_major float4x4 ModelViewProj : register(c0);
row_major float3x3 TexCoordTranform : register(c4); // the engine's spelling; rows 0 and 1 (c4, c5) are used
float4 FogParam : register(c13);                     // x: fog start, y: fog range, z: fog power

struct VS_INPUT {
    float4 position : POSITION;
    float4 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VS_OUTPUT {
    float4 sPosition : POSITION;
    float4 uv : TEXCOORD0;             // xy: texture, z: 1 (alpha scale), w: fog
    float4 color : COLOR0;
    float4 shadowWorldPos : TEXCOORD1; // xyz: camera-relative world position, w: SHADOW_VS_SENTINEL
    float4 vertexLight : TEXCOORD2;    // rgb: ambient + point lights (ParticleLight.hlsl)
};

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float4 position = mul(ModelViewProj, IN.position);
    OUT.sPosition = position;

    float3 uv = float3(IN.uv.xy, 1.0f);
    OUT.uv.x = dot(TexCoordTranform[0], uv);
    OUT.uv.y = dot(TexCoordTranform[1], uv);
    OUT.uv.z = 1.0f;
    OUT.uv.w = pow(1.0f - saturate((FogParam.x - length(position.xyz)) / FogParam.y), FogParam.z);

    OUT.color = IN.color;
    float3 worldPos = GetShadowWorldPos(position);
    OUT.shadowWorldPos = float4(worldPos, SHADOW_VS_SENTINEL);
    OUT.vertexLight = float4(ParticleVertexLight(worldPos), 1.0f);
    return OUT;
}
