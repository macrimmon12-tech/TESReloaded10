// Rain fullscreen shader for Oblivion Reloaded
string PipelinePosition = "PostTonemapping";

#define RainLayers 15

float4x4 TESR_WorldViewProjectionTransform
<
	string widget = "hidden";
	string name = "World-View-Projection Transform";
	string description = "Combined world-view-projection matrix, supplied by the engine for placing the rain streak geometry. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4x4 TESR_ShadowCameraToLightTransformOrtho
<
	string widget = "hidden";
	string name = "Shadow Ortho Camera-To-Light Transform";
	string description = "ShadowsExteriorEffect's own registered ortho-cascade transform (see ShadowsExteriors.fx.hlsl), used here to sample the ortho shadow map for rain occlusion. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_ReciprocalResolution
<
	string widget = "hidden";
	string name = "Reciprocal Resolution";
	string description = "Per-frame render target metrics supplied by the engine: x = 1/width, y = 1/height, z = aspect ratio (width/height), w = reserved for FoV. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_GameTime
<
	string widget = "hidden";
	string name = "Game Time";
	string description = "Per-frame game clock supplied by the engine: x = time in milliseconds, y = time in hours (0-24), z = frame time counter, w = elapsed time in seconds since last frame. Not user-configurable.";
	float defaultValue = 0.0;
>;
// x (current rain amount) is deliberately left out of componentKeys below:
// it's an animated fade value, not a setting.
float4 TESR_RainData
<
	string widget = "packed";
	string name = "Rain Data";
	string description = "Packed rain parameters: x = current rain amount (0-1, animated fade in/out with weather), y = VerticalScale (Shaders.Precipitations.Main.VerticalScale), z = Speed, w = Opacity.";
	string componentKeys     = "VerticalScale,Speed,Opacity";
	string componentDefaults = "1.6,3.0,1.0";
	string componentMins     = "0,0,0";
	string componentMaxs     = "5,10,1";
	string componentSteps    = "0.01,0.1,0.01";
	float defaultValue = 0.0;
>;
float4 TESR_RainAspect
<
	string widget = "packed";
	string name = "Rain Aspect";
	string description = "Packed rain visual parameters (Shaders.Precipitations.Main): x = Refraction (screen distortion amount), y = Coloring (base coloring amount added to the refracted color), z = Bloom (rain blocked-by-objects glow).";
	string componentKeys     = "Refraction,Coloring,Bloom";
	string componentDefaults = "1.0,1.1,0.5";
	string componentMins     = "0,0,0";
	string componentMaxs     = "5,3,2";
	string componentSteps    = "0.01,0.01,0.01";
	float defaultValue = 1.0;
>;
float4 TESR_FogColor
<
	string widget = "hidden";
	string name = "Fog Color";
	string description = "Current weather fog color (RGB), supplied by the engine. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunDirection
<
	string widget = "hidden";
	string name = "Sun Direction";
	string description = "World-space direction vector to the sun/moon light source, normalized, supplied by the engine. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunColor
<
	string widget = "hidden";
	string name = "Sun Color";
	string description = "Current directional sunlight color (RGB), supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_RenderedBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_OrthoMapBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

static const float PI = 3.14159265;
static const float timetick = TESR_GameTime.z;
static const float hscale = 0.004f * TESR_RainData.y; // low values stretch the volume vertically
static const float3x3 p = float3x3(30.323122,30.323122,30.323122,30.323122,30.323122,30.323122,30.323122,30.323122,30.323122);
static const float DEPTH = 100; // distance step between each layer
static const float SPEED = TESR_RainData.z; // speed multiplier for falling animation 

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

#include "Includes/Depth.hlsl"
#include "Includes/Helpers.hlsl"

// convert world position to a coordinate on a cylinder around the player
float2 cylindrical(float3 world)
{
	float u = -atan2(world.y, world.x) / PI; 
	float v = world.z / length(world.xy);
	return float2(compress(u), hscale * v);
}

// checks wether a point is underneath something occluding
float GetOrtho(float4 OrthoPos) {	
	OrthoPos.xyz /= OrthoPos.w;
    float outofbounds = (OrthoPos.x < -1.0f || OrthoPos.x > 1.0f ||
        OrthoPos.y < -1.0f || OrthoPos.y > 1.0f ||
        OrthoPos.z <  0.0f || OrthoPos.z > 1.0f);
 
    OrthoPos.x = OrthoPos.x *  0.5f + 0.5f;
    OrthoPos.y = OrthoPos.y * -0.5f + 0.5f;
	float Ortho = tex2D(TESR_OrthoMapBuffer, OrthoPos.xy).r;
	return (Ortho > OrthoPos.z - 0.0001) * (1 - outofbounds); 
}

float4 Rain( VSOUT IN ) : COLOR0
{
	float4 color = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	int iterations = RainLayers;

	// calculating the ray along which the  volumetric rain will be calculated
	
    float3 rayStart = toWorld(IN.UVCoord);
    float3 rayEnd = rayStart * DEPTH * iterations;
    float4 orthoStart = mul(float4(rayStart.xyz, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    float4 orthoEnd = mul(float4(rayEnd.xyz, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    float4 step = (orthoEnd - orthoStart) / iterations;
	
	// each iteration adds a cylindrical layer of drops
    float depth = readDepth(IN.UVCoord);
	float2 uv = cylindrical(rayStart.xyz); // converts world coordinates to cylinder coordinates around the player

	float totalRain = 0.0f;

	[unroll]
	for (int i = 1; i < iterations; i++){
		float2 noiseSurfacePosition = uv * (1.0f + i * DEPTH); // scale cylinder coordinates with iterations
		noiseSurfacePosition += float2(noiseSurfacePosition.y * (fmod(i * 107.238917f, 1.0f) - 0.5f), SPEED * timetick); // animate y offset, not sure what x does?

		// creating the noise with magic
		float3 m = float3(floor(noiseSurfacePosition), i); // places the drops at the same distance from the camera?
		float3 mp = m/frac(mul(p, m)); // bayer matrix? grid of grey values stretch depending on the world positions
		float3 rainNoise = frac(mp); //r.x is a random generated grid of different darknesses

        // determin drop shape
		float2 shape = abs(fmod(noiseSurfacePosition, 1.0f) + rainNoise.xy - 1.0f);
		shape += 0.01f * abs(2.0f * frac(10.0f * noiseSurfacePosition.yx) - 1.0f); // shape is a horizontal gradient on each square 
		float dropGradient = 0.6f * max(shape.x - shape.y, shape.x + shape.y) + max(shape.x, shape.y) - 0.01f; // creates a circular gradient on each square of the noise grid to focus the drop
		float edge = 0.005f + 0.05f * min(1.0f * i, 4.0f * TESR_RainData.x); // can't be 0 or smoothstep will fail. the smaller this value, the smaller the drops

		// add new drop influence to the fragment, by extracting the brightest spots in the gradients grid using the edge parameter
		float drop = smoothstep(edge, -edge, dropGradient) * (rainNoise.x / (1.0f + 0.2f * i)); // fade opacity with each iteration
		drop *= GetOrtho(orthoStart + step * i) * (depth > DEPTH * i); // depth and ortho check for rain
		totalRain += drop;
	}

	// a rain tint color that scales with the sun direction
	float4 sunColor = linearize(TESR_SunColor);
	float4 rainColor = lerp(float(0.5).xxxx, sunColor * 2, pow(shades(normalize(rayStart.xyz), TESR_SunDirection.xyz), 2));

	// sample the bloom buffer and the source buffer with refracted UV to shade the rain with
	float2 refractedUV = IN.UVCoord + float2(totalRain * TESR_RainAspect.x, -totalRain * TESR_RainAspect.x);
	float4 refractedColor = linearize(tex2D(TESR_SourceBuffer, refractedUV));
	refractedColor += tex2D(TESR_BloomBuffer, refractedUV);

	return delinearize(lerp(color, refractedColor + rainColor * TESR_RainAspect.y * 0.02, totalRain* TESR_RainData.w));
}








technique
{

	
	

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Rain();
	}
}