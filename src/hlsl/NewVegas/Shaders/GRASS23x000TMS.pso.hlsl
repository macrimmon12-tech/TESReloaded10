// Grass PS -- vanilla GRASS23x000TMS.pso plus forward sun shadow and grass lighting.
// Shared by GRASS23x002.vso (LFS) and GRASS23x003.vso (LVS).
//
// The VS hands the lighting over already split, so only the sun is shadowed:
//   TEXCOORD4.xyz ambient    TEXCOORD5.xyz sun    TEXCOORD5.w fade    COLOR0 fog (.w amount)
// and, for the grass lighting below, the raw pieces to redo the sun per pixel:
//   TEXCOORD2 sun normal + root-to-tip    TEXCOORD3 offset from the clump centre
//   TEXCOORD6 sun colour before N.L       TEXCOORD7 sun direction
//
// Grass lighting. Vanilla lights a whole clump as one flat card facing its up direction, so a field
// is one tone wherever the sun is. Four additions, each off at 0 (and all at 0 is vanilla exactly):
//   - Roundness bends each blade's normal outward from the clump's centre, so the side of a clump
//     facing away from the sun falls into shade and fields gain depth.
//   - Root darkening shades blades toward the ground, using the vertex alpha the wind already uses
//     as its root-to-tip weight (0 at the root, which is why roots do not sway).
//   - Translucency lets sunlight through the blades when the sun is behind them: the glow of a
//     backlit field. Coloured by the grass texture, like light through a leaf, and shadowed.
//   - Specular adds a soft sheen off the rounded normals toward the sun.

#include "includes/Shadow.hlsl"
#include "includes/PBRScale.hlsl"

// c145/c146: past PBRScale/SkyAmbient's c134-c144.
float4 TESR_GrassLighting  : register(c145); // x: translucency, y: roundness, z: root darkening, w: specular
float4 TESR_GrassLighting2 : register(c146); // x: translucency focus, y: specular glossiness

sampler2D DiffuseMap : register(s0);

struct PS_INPUT {
    float2 uv             : TEXCOORD0;
    float4 shadowWorldPos : TEXCOORD1;
    float4 blade          : TEXCOORD2_centroid;   // xyz: sun normal, w: root (0) to tip (1)
    float3 bladeOffset    : TEXCOORD3_centroid;
    float3 ambient        : TEXCOORD4_centroid;
    float4 sun            : TEXCOORD5_centroid;   // .w = distance fade
    float3 sunColor       : TEXCOORD6_centroid;
    float3 sunDir         : TEXCOORD7_centroid;
    float4 fog            : COLOR0;               // .w = fog amount
};

struct PS_OUTPUT {
    float4 color : COLOR0;
};

PS_OUTPUT main(PS_INPUT IN) {
    PS_OUTPUT OUT;

    float translucency = TESR_GrassLighting.x;
    float roundness = TESR_GrassLighting.y;
    float rootDarkening = TESR_GrassLighting.z;
    float specular = TESR_GrassLighting.w;
    float translucencyFocus = max(TESR_GrassLighting2.x, 1.0f);
    float gloss = max(TESR_GrassLighting2.y, 1.0f);

    // Outside the guard: the skylight needs this normal whether or not forward shadows
    // are compiled in, and ForwardShadows is a live setting that can switch them off.
    float3 shadowNormal = GetShadowGeometricNormal(IN.shadowWorldPos.xyz);
    float present = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
    float shadow = 1.0f;
#if FORWARD_SHADOWS
    // ddx/ddy must stay at top level, outside dynamic flow control.
    shadow = present > 0.5f
         ? GetSunShadow(IN.shadowWorldPos.xyz, shadowNormal)
         : 1.0f;
#endif

    float3 L = normalize(IN.sunDir);
    float3 V = normalize(IN.shadowWorldPos.xyz);   // camera-relative world position: camera to pixel

    // Rounded normal: the variant's sun normal tipped outward by the blade's offset from the clump
    // centre. The 8-unit softening keeps the centre from flipping direction on a tiny offset.
    float3 offset = IN.bladeOffset;
    float3 N = normalize(normalize(IN.blade.xyz) + roundness * offset / (length(offset) + 8.0f));

    // At Roundness 0 keep the vertex shader's own N.L, so vanilla stays vanilla to the bit.
    float3 sun = roundness > 0.0f ? IN.sunColor * saturate(dot(L, N)) : IN.sun.xyz;
    sun *= shadow;

    // Translucency: strongest looking straight toward the sun, narrowed by the focus exponent, and
    // weighted toward the tips, where blades are thinnest.
    float tip = saturate(IN.blade.w);
    float through = pow(saturate(dot(V, L)), translucencyFocus) * translucency * (0.5f + 0.5f * tip) * present;
    float3 transmitted = IN.sunColor * shadow * through;

    // Root darkening: lets the roots sit in their own shade.
    float ao = lerp(1.0f - rootDarkening, 1.0f, tip);

    // Same split getSunLighting/getAmbientLighting apply on the object path.
    float3 lighting = (PBRLight(sun + transmitted) + PBRAmbient(IN.ambient.xyz) + SkyAmbient(shadowNormal, present)) * ao;

    float4 albedo = tex2D(DiffuseMap, IN.uv.xy);
    float3 litColor = lighting * albedo.rgb;

    // Sheen: Blinn-Phong off the rounded normal, in the light's colour rather than the texture's.
    // Normalised by hand: L - V is zero looking exactly into the sun, and normalize() would NaN.
    float3 H = L - V;
    H *= rsqrt(max(dot(H, H), 1e-8f));
    litColor += PBRLight(IN.sunColor * shadow) * pow(saturate(dot(N, H)), gloss) * specular * present;

    OUT.color.rgb = lerp(litColor, IN.fog.rgb, IN.fog.w);
    OUT.color.a = saturate(albedo.a * 1.75f) * IN.sun.w;

    return OUT;
};
