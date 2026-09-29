// Shader to compute the complete point-light shadow pass in one screen draw.

float4 TESR_ShadowLightPosition[12];
float4 TESR_ShadowLightWeight[12]; // x: how much of the slot's light is shadowed, y: how much of the light is counted
float4 TESR_LightPosition[12];
float4 TESR_LightColor[24];
float4 TESR_ShadowFade;
float4 TESR_SpotLightPosition;
float4 TESR_SpotLightDirection;
float4 TESR_SpotLightColor;


//sampler_state removed to avoid a artifact. TODO investigate
sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer0 : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer1 : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer2 : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer3 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer4 : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer5 : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer6 : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer7 : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer8 : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer9 : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer10 : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/Shadows.hlsl"

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}


float GetSpotLightAmount(float4 worldPos, float4 spotLightPosition, float4 spotLightDirection, float4 normal){
    float3 lightToWorld = spotLightPosition.rgb - worldPos.xyz;
    float3 lightVector = normalize(lightToWorld);

	// radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
    float radius = spotLightPosition.w;
    float Distance = length(lightToWorld)/radius;
	float s = saturate(Distance * Distance); 
	float atten = saturate(((1 - s) * (1 - s)) / (1 + 5.0 * s));

	float angleCosMax = cos(radians(spotLightDirection.w));
	float angleCosMin = cos(radians(spotLightDirection.w * 0.5));
	float cone = pow(invlerps(angleCosMax, angleCosMin, shades(spotLightDirection.xyz, lightVector * -1)), 2.0);

    float diffuse = shade(lightVector, normal.xyz);
	return diffuse * cone * atten;
}


// One shadow slot. While the slot passes from one light to another, the shadow weight blends the light with its cubemap
// shadow towards the same light without it. At weight 1 this is exactly GetPointLightAmount.
float SlotLight(samplerCUBE cubeMap, float4 worldPos, float4 lightPos, float4 normal, float4 color, float shadowWeight) {
	float shadowed = GetPointLightAmount(cubeMap, worldPos, lightPos, normal);
	float unshadowed = lightPos.w ? GetPointLightContribution(worldPos, lightPos, normal) : 0.0f;
	return (shadowed + (unshadowed - shadowed) * (1.0f - shadowWeight)) * luma(color.rgb) * color.w;
}


float4 Shadow( VSOUT IN ) : COLOR0 {

	float2 uv = IN.UVCoord;
	float depth = readDepth(uv);
    float3 camera_vector = toWorld(uv) * depth;
    float4 world_pos = float4(TESR_CameraPosition.xyz + camera_vector, 1.0f);	
	float4 normal = float4(GetWorldNormal(uv), 1);
	// float Shadow = 0.0;

	float Shadow = SlotLight(TESR_ShadowCubeMapBuffer0, world_pos, TESR_ShadowLightPosition[0], normal, TESR_LightColor[0], TESR_ShadowLightWeight[0].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer1, world_pos, TESR_ShadowLightPosition[1], normal, TESR_LightColor[1], TESR_ShadowLightWeight[1].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer2, world_pos, TESR_ShadowLightPosition[2], normal, TESR_LightColor[2], TESR_ShadowLightWeight[2].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer3, world_pos, TESR_ShadowLightPosition[3], normal, TESR_LightColor[3], TESR_ShadowLightWeight[3].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer4, world_pos, TESR_ShadowLightPosition[4], normal, TESR_LightColor[4], TESR_ShadowLightWeight[4].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer5, world_pos, TESR_ShadowLightPosition[5], normal, TESR_LightColor[5], TESR_ShadowLightWeight[5].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer6, world_pos, TESR_ShadowLightPosition[6], normal, TESR_LightColor[6], TESR_ShadowLightWeight[6].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer7, world_pos, TESR_ShadowLightPosition[7], normal, TESR_LightColor[7], TESR_ShadowLightWeight[7].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer8, world_pos, TESR_ShadowLightPosition[8], normal, TESR_LightColor[8], TESR_ShadowLightWeight[8].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer9, world_pos, TESR_ShadowLightPosition[9], normal, TESR_LightColor[9], TESR_ShadowLightWeight[9].x);
	Shadow += SlotLight(TESR_ShadowCubeMapBuffer10, world_pos, TESR_ShadowLightPosition[10], normal, TESR_LightColor[10], TESR_ShadowLightWeight[10].x);
	[branch] if (TESR_ShadowLightPosition[11].w)
		Shadow += GetPointLightContribution(world_pos, TESR_ShadowLightPosition[11], normal) * TESR_ShadowLightWeight[11].y;

	[branch] if (TESR_SpotLightPosition.w)
		Shadow += GetSpotLightAmount(world_pos, TESR_SpotLightPosition, TESR_SpotLightDirection, normal) * luma(TESR_SpotLightColor.rgb) * TESR_SpotLightColor.w;
	
	for (int i = 0; i< 12; i++){
		[branch] if (TESR_LightPosition[i].w)
			Shadow += GetPointLightContribution(world_pos, TESR_LightPosition[i], normal) * luma(TESR_LightColor[i + 12].rgb) * TESR_LightColor[i + 12].w;
	}

	Shadow = saturate(Shadow);
	
	return float4(Shadow, Shadow, Shadow, 1.0f);
}

technique MergedPointShadows {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow();
	}
}
