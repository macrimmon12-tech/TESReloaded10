// SpeedTree leaf PS shared parts.
// TEXCOORD1: vanilla combined lighting. TEXCOORD3: the sun-only part of it.
// Subtracting the occluded fraction makes shadow = 1 bit-identical to vanilla.
#ifndef LEAF_INCLUDED
#define LEAF_INCLUDED

sampler2D DiffuseMap : register(s0);

struct PS_INPUT {
    float2 uv             : TEXCOORD0;
    float4 lighting       : TEXCOORD1_centroid;
    float4 fog            : TEXCOORD2_centroid;   // rgb colour, w amount
    float3 sun            : TEXCOORD3_centroid;
    float4 shadowWorldPos : TEXCOORD4;
};

struct PS_OUTPUT {
    float4 color : COLOR0;
};

// Top level of main only: ddx/ddy are illegal under dynamic flow control.
float3 LeafLighting(PS_INPUT IN) {
#if FORWARD_SHADOWS
    float3 n = GetShadowGeometricNormal(IN.shadowWorldPos.xyz);
    float s = SHADOW_VS_PRESENT(IN.shadowWorldPos.w)
            ? GetSunShadow(IN.shadowWorldPos.xyz, n)
            : 1.0f;
    return IN.lighting.rgb - IN.sun * (1.0f - s);
#else
    return IN.lighting.rgb;
#endif
}

#endif
