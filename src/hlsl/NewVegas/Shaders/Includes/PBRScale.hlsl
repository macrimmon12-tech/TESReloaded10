// PBR light/ambient scales for the hand-ported game shaders (skin, hair, grass).
// Object.hlsl applies the same .z and .w in getSunLighting / getPointLightLighting /
// getAmbientLighting.
//
// c134, past Shadow.hlsl's c100-c133. Object.hlsl must not be included here: it declares
// TESR_PBRData at c32.
//
// DEPENDENCY: ShaderManager::UpdateConstants refreshes PBRShaders unconditionally. A zero
// scale renders everything below black.
#ifndef PBRSCALE_INCLUDED
#define PBRSCALE_INCLUDED

float4 TESR_PBRData : register(c134);

// rgb only: skin carries flags in AmbientColor.a and PSLightColor.a.
float3 PBRLight(float3 lightColor) { return lightColor * TESR_PBRData.z; }
float4 PBRLight(float4 lightColor) { return float4(lightColor.rgb * TESR_PBRData.z, lightColor.a); }

float3 PBRAmbient(float3 ambient) { return ambient * TESR_PBRData.w; }
float4 PBRAmbient(float4 ambient) { return float4(ambient.rgb * TESR_PBRData.w, ambient.a); }

#endif
