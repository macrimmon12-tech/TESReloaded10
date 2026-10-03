// UNOFFICIAL lit particles (Shaders.Particles, off by default): vanilla NOLIGHT017.vso, particle systems with an
// animated texture sheet (BSSM_NOLIGHTING_PSYS_SUBTEX_OFFSET and its premultiplied-alpha twin). The game's vs_2_0
// shader ported as is, plus the camera-relative world position (and the sentinel) for the lit NOLIGHTTEXVC.pso.
// Registers as in the game's shader: the engine sets them by number.

#include "includes/Shadow.hlsl"
#define PARTICLE_VS
#include "includes/ParticleLight.hlsl"

row_major float4x4 ModelViewProj : register(c0);
float4 FogParam : register(c13);         // x: fog start, y: fog range, z: fog power
float4 SubTexOffsets[16] : register(c15); // per frame of the sheet: xz offset, yw scale

struct VS_INPUT {
    float4 position : POSITION;
    float4 uv : TEXCOORD0;
    float4 frame : TEXCOORD1; // x: frame of the texture sheet
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

    float4 frame = SubTexOffsets[(int)(IN.frame.x - frac(IN.frame.x))];
    OUT.uv.xy = frame.yw * IN.uv.xy + frame.xz;
    OUT.uv.z = 1.0f;
    OUT.uv.w = pow(1.0f - saturate((FogParam.x - length(position.xyz)) / FogParam.y), FogParam.z);

    OUT.color = IN.color;
    float3 worldPos = GetShadowWorldPos(position);
    OUT.shadowWorldPos = float4(worldPos, SHADOW_VS_SENTINEL);
    OUT.vertexLight = float4(ParticleVertexLight(worldPos), 1.0f);
    return OUT;
}
