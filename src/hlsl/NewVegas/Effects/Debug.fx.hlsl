string PipelinePosition = "PostTonemapping";

// Debug is a developer diagnostic overlay (Main.Develop.Main.DebugMode) that
// displays several of the engine's own buffers/constants -- it has no tunable
// settings of its own besides the DebugVar scratch values.
float4 TESR_SunAmbient
<
	string widget = "hidden";
	string name = "Sun Ambient";
	string description = "Current ambient sky light color (RGB), supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunColor
<
	string widget = "hidden";
	string name = "Sun Color";
	string description = "Current directional sunlight color (RGB), supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_FogColor
<
	string widget = "hidden";
	string name = "Fog Color";
	string description = "Current weather fog color (RGB), supplied by the engine. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_WaterShallowColor
<
	string name = "Water Shallow Color";
	string description = "Shallow-water color, supplied by the game's water form. Not user-configurable.";
	string widget = "hidden";
	float3 defaultValue = {0.0, 0.0, 0.0};
>;
float4 TESR_WaterDeepColor
<
	string name = "Water Deep Color";
	string description = "Deep-water color, supplied by the game's water form. Not user-configurable.";
	string widget = "hidden";
	float3 defaultValue = {0.0, 0.0, 0.0};
>;
float4 TESR_HorizonColor
<
	string widget = "hidden";
	string name = "Horizon Color";
	string description = "Horizon color, supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SkyColor
<
	string widget = "hidden";
	string name = "Sky Color";
	string description = "Top-of-sky color, supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_ViewSpaceLightDir
<
	string widget = "hidden";
	string name = "View Space Light Direction";
	string description = "Sun/moon light direction transformed into view space, supplied by the engine. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunDirection
<
	string widget = "hidden";
	string name = "Sun Direction";
	string description = "World-space direction vector to the sun/moon light source, normalized, supplied by the engine. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_DebugVar
<
	string name = "Debug Variable";
	string description = "This effect's own registered constant (Main.Develop.Main.DebugVar1-4): four free developer scratch values. Not intended for normal use.";
	float defaultValue = 0.0;
>;
float4 TESR_ShadowRadius
<
	string widget = "hidden";
	string name = "Shadow Radius";
	string description = "ShadowsExteriorEffect's own registered constant: per-cascade shadow map radius (near/middle/far/lod), used here to visualize cascade boundaries by depth. Not user-configurable.";
	float defaultValue = 0.0;
>;

float4 TESR_LightPosition[12]
<
	string widget = "hidden";
	string name = "Light Positions";
	string description = "World-space position + radius of up to 12 nearby point lights, supplied by the engine each frame. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_ShadowLightPosition[12]
<
	string widget = "hidden";
	string name = "Shadow Light Positions";
	string description = "ShadowsExteriorEffect's own registered constant: world-space position + radius of up to 12 point lights currently casting shadows. Not user-configurable.";
	float defaultValue = 0.0;
>;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_AvgLumaBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_BloomBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_SMAA_Edges : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SMAA_Blend : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };


struct VSOUT {
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN {
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN) {
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"


float4 displayBuffer(float4 color, float2 uv, float2 bufferPosition, float2 bufferSize, sampler2D buffer){
	float2 lowerCorner = bufferPosition + bufferSize;
	if ((uv.x < bufferPosition.x || uv.y < bufferPosition.y) || (uv.x > lowerCorner.x || uv.y > lowerCorner.y )) return color;
	return tex2D(buffer, float2(invlerp(bufferPosition, lowerCorner, uv)));
}

float4 displayDepth(float4 color, float2 uv, float2 bufferPosition, float2 bufferSize){
	float2 lowerCorner = bufferPosition + bufferSize;
	if ((uv.x < bufferPosition.x || uv.y < bufferPosition.y) || (uv.x > lowerCorner.x || uv.y > lowerCorner.y )) return color;
	return pows(readDepth(float2(invlerp(bufferPosition, lowerCorner, uv))) / farZ, 0.3);
}

float4 displayShadows(float4 color, float2 uv, float2 bufferPosition, float2 bufferSize){
	float2 lowerCorner = bufferPosition + bufferSize;
	if ((uv.x < bufferPosition.x || uv.y < bufferPosition.y) || (uv.x > lowerCorner.x || uv.y > lowerCorner.y )) return color;
	float4 Shadows = tex2D(TESR_PointShadowBuffer, float2(invlerp(bufferPosition, lowerCorner, uv)));
	return Shadows.rrrr * 0.5 + Shadows.gggg;
}

float4 displayBloom(float4 color, float2 uv, float2 bufferPosition, float2 bufferSize, sampler2D buffer){
	float2 lowerCorner = bufferPosition + bufferSize;
	if ((uv.x < bufferPosition.x || uv.y < bufferPosition.y) || (uv.x > lowerCorner.x || uv.y > lowerCorner.y )) return color;
	return tex2D(buffer, float2(invlerp(bufferPosition, lowerCorner, uv)));
}

float4 showLightInfluence(float4 color, float2 uv, float3 position, float4 lightPos, float4 tint){
	float3 eyeDir = normalize(toWorld(uv));

	float3 lightDir = lightPos.xyz - TESR_CameraPosition.xyz;
	float lightDist = length(lightDir);
	lightDir = normalize(lightDir);

	// points for light positions
	float distance = saturate(1 - lightDist / 5000);
	if (shades(eyeDir, lightDir) > (1 - distance * 0.0001)) return tint;

	// shade area of influence
	distance = length(position - lightPos.xyz) / lightPos.w;
	distance *= distance;
	color += saturate((1 - distance) / (1 + distance)) * tint/5;
	//if (lightPos.w > 0 && lightDist < lightPos.w) color = saturate(color + float4(1, 0, 0, 1) * shades(eyeDir, lightDir));

	return color;
}

float4 DebugShader( VSOUT IN) : COLOR0 {
    
	float depth = readDepth(IN.UVCoord);
	float3 eyeDir = toWorld(IN.UVCoord);
	float3 position = eyeDir * depth + TESR_CameraPosition.xyz;


	float4 color = tex2D(TESR_RenderedBuffer, IN.UVCoord);
	// if (depth > 0 && depth < TESR_ShadowRadius.x) color *= float4(1, 0.5, 1, 1);
	// if (depth > TESR_ShadowRadius.x && depth < TESR_ShadowRadius.y) color *= float4(0.5, 1, 1, 1);
	// if (depth > TESR_ShadowRadius.y && depth < TESR_ShadowRadius.z) color *= float4(1, 1, 0.5, 1);
	// if (depth > TESR_ShadowRadius.z && depth < TESR_ShadowRadius.w) color *= float4(0.5, 1, 1, 1);

	for (int i=0; i<12; i++){
		float4 tint = i<7?green:blue;
		color = showLightInfluence(color, IN.UVCoord, position, TESR_ShadowLightPosition[i], tint);
	}
	for (i=0; i<12; i++){
		color = showLightInfluence(color, IN.UVCoord, position, TESR_LightPosition[i], red);
	}

    if (IN.UVCoord.x > 0.9 && IN.UVCoord.x < 0.95){
        if (IN.UVCoord.y > 0.15 && IN.UVCoord.y < 0.2) return TESR_SunColor;
        if (IN.UVCoord.y > 0.2 && IN.UVCoord.y < 0.25) return TESR_SunAmbient;
        if (IN.UVCoord.y > 0.25 && IN.UVCoord.y < 0.3) return TESR_FogColor;
        if (IN.UVCoord.y > 0.3 && IN.UVCoord.y < 0.35) return TESR_HorizonColor;
        if (IN.UVCoord.y > 0.35 && IN.UVCoord.y < 0.4) return TESR_SkyColor;
        if (IN.UVCoord.y > 0.4 && IN.UVCoord.y < 0.45) return TESR_WaterShallowColor;
        if (IN.UVCoord.y > 0.45 && IN.UVCoord.y < 0.5) return TESR_WaterDeepColor;
        if (IN.UVCoord.y > 0.55 && IN.UVCoord.y < 0.6) return tex2D(TESR_AvgLumaBuffer, float2(0.5, 0.5));
        if (IN.UVCoord.y > 0.65 && IN.UVCoord.y < 0.7) return TESR_ViewSpaceLightDir;
        if (IN.UVCoord.y > 0.7 && IN.UVCoord.y < 0.75) return TESR_SunDirection;
    }

	color = displayBuffer(color, IN.UVCoord, float2(0.1, 0.05), float2(0.15, 0.15), TESR_NormalsBuffer);
	color = displayDepth(color, IN.UVCoord, float2(0.3, 0.05), float2(0.15, 0.15));
	color = displayShadows(color, IN.UVCoord, float2(0.5, 0.05), float2(0.15, 0.15));
	color = displayBloom(color, IN.UVCoord, float2(0.7, 0.05), float2(0.15, 0.15), TESR_BloomBuffer);
    color = displayBuffer(color, IN.UVCoord, float2(0.1, 0.25), float2(0.15, 0.15), TESR_SMAA_Edges);
    color = displayBuffer(color, IN.UVCoord, float2(0.3, 0.25), float2(0.15, 0.15), TESR_SMAA_Blend);

    return float4(color.rgb, 1);
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DebugShader();
	}
}