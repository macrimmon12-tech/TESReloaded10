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

// How far towards the sun a leaf's shadow lookup starts, in world units.
//
// SpeedTree leaf cards turn to face the camera on screen but the sun in the shadow map
// (ShadowManager::RecalculateBillboardVectors), so a leaf looked up where it is drawn is tested
// against its own card and its neighbours standing at another angle. That covered bushes in dark
// streaks. Moved this far along the sun, the lookup leaves out every occluder nearer than that -
// the plant's own leaves and branches - and still counts the ones further away. At 100 the lower
// half of a bush stayed streaked; 200 cleared it, and bushes in a building's shade stayed dark.
#define LEAF_SHADOW_REACH 200.0f

float3 LeafLighting(PS_INPUT IN) {
#if FORWARD_SHADOWS
    // The sun stands in for the normal, which leaves the lookup without a slope bias - a card
    // turned to the camera says nothing about the surface the sun sees.
    float3 sunDir = TESR_SmoothedSunDir.xyz;
    float s = SHADOW_VS_PRESENT(IN.shadowWorldPos.w)
            ? GetSunShadow(IN.shadowWorldPos.xyz + sunDir * LEAF_SHADOW_REACH, sunDir)
            : 1.0f;
    return IN.lighting.rgb - IN.sun * (1.0f - s);
#else
    return IN.lighting.rgb;
#endif
}

#endif
